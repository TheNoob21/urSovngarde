#include <Services/ReferenceStateService.h>

#include <World.h>

#include <Events/ActivateEvent.h>
#include <Events/BearTrapEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>

#include <Messages/NotifyReferenceStates.h>
#include <Messages/RequestReferenceState.h>
#include <Messages/RequestSaveLoaded.h>

#include <Services/ChestSelection.h>
#include <Services/PapyrusService.h>
#include <Services/TransportService.h>

#include <Games/Overrides.h>
#include <Games/TES.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <PlayerCharacter.h>
#include <Interface/UI.h>

struct BGSKeyword;

namespace
{
using ObjectReference = TESObjectREFR;

//! TESObjectREFR's flag for a picked plant (SKSE's kFlag_Harvested), kept in the save by the change flag for an emptied
//! reference (kChange_IsEmpty), as SKSE's SetHarvested sets both.
constexpr uint32_t kHarvestedFlag = 0x2000;
constexpr uint32_t kChangeEmpty = 0x00200000;

constexpr uint8_t kTreeType = 38;
constexpr uint8_t kFloraType = 39;
constexpr uint8_t kFurnitureType = 40;

//! A destructible's stage change this long after it was brought to another game's stage is that game's.
constexpr auto kSettledWindow = std::chrono::seconds(3);
//! The loaded cells are looked through for rune traps, and the watched references checked, this often.
constexpr auto kRuneWatchInterval = std::chrono::seconds(2);

bool IsPlant(const TESObjectREFR* apRef) noexcept
{
    const uint8_t cType = static_cast<uint8_t>(apRef->baseForm->formType);
    return cType == kTreeType || cType == kFloraType;
}

bool IsAttachedRef(const TESObjectREFR* apRef) noexcept
{
    const TESObjectCELL* pCell = apRef ? apRef->GetParentCellEx() : nullptr;
    return pCell && pCell->IsAttached() && apRef->baseForm && !apRef->IsDeleted();
}

bool IsPlaying() noexcept
{
    static const BSFixedString s_loadingMenu("Loading Menu");
    static const BSFixedString s_mainMenu("Main Menu");
    const UI* pUI = UI::Get();
    return pUI && PlayerCharacter::Get() && !pUI->GetMenuOpen(s_loadingMenu) && !pUI->GetMenuOpen(s_mainMenu);
}

TESObjectREFR* GetRef(uint32_t aFormId) noexcept
{
    return Cast<TESObjectREFR>(TESForm::GetById(aFormId));
}

uint32_t PluginPrefix(const char* acpPlugin) noexcept
{
    const Mod* pPlugin = ModManager::Get()->GetByName(acpPlugin);
    return pPlugin && pPlugin->IsLoaded() && !pPlugin->IsLite() ? pPlugin->standardId : 0x100;
}

int32_t CurrentStage(const TESObjectREFR* apRef) noexcept
{
    PAPYRUS_FUNCTION(int32_t, ObjectReference, GetCurrentDestructionStage);
    return s_pGetCurrentDestructionStage(apRef);
}
} // namespace

ReferenceStateService::ReferenceStateService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&ReferenceStateService::OnUpdate>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&ReferenceStateService::OnDisconnected>(this);
    m_activateConnection = aDispatcher.sink<ActivateEvent>().connect<&ReferenceStateService::OnActivate>(this);
    m_bearTrapConnection = aDispatcher.sink<BearTrapEvent>().connect<&ReferenceStateService::OnBearTrap>(this);
    m_notifyConnection = aDispatcher.sink<NotifyReferenceStates>().connect<&ReferenceStateService::OnNotifyReferenceStates>(this);

    EventDispatcherManager::Get()->destructionStageChangedEventDispatcher.RegisterSink(this);
    EventDispatcherManager::Get()->loadGameEvent.RegisterSink(this);
}

void ReferenceStateService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    ForgetTracked();
}

BSTEventResult ReferenceStateService::OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*)
{
    m_saveLoaded = true;
    return BSTEventResult::kOk;
}

void ReferenceStateService::ForgetTracked() noexcept
{
    m_stages.Clear();
    m_pending.Clear();
    m_uses.Clear();
    m_bearTraps.ClearWanted();
    m_settled.clear();
    m_resets.Clear();
    m_outbox.Clear();
}

// Fired on the game's own threads: handled with the world's update.
BSTEventResult ReferenceStateService::OnEvent(const TESDestructionStageChangedEvent* acpEvent, const EventDispatcher<TESDestructionStageChangedEvent>*)
{
    if (acpEvent && acpEvent->pTarget)
        m_world.GetRunner().Queue([this, cFormId = acpEvent->pTarget->formID]() { OnStageChanged(cFormId); });
    return BSTEventResult::kOk;
}

void ReferenceStateService::OnStageChanged(uint32_t aFormId) noexcept
{
    if (!m_transport.IsConnected())
        return;
    TESObjectREFR* pRef = GetRef(aFormId);
    if (!pRef || pRef->IsTemporary() || !IsAttachedRef(pRef) || !IsSharedDestructible(pRef))
        return;

    // Brought to another game's stage here: that game's change.
    const auto cNow = std::chrono::steady_clock::now();
    if (const auto cIt = m_settled.find(aFormId); m_stages.IsSettling(aFormId) || (cIt != m_settled.end() && cNow - cIt->second < kSettledWindow))
        return;

    const int32_t cStage = CurrentStage(pRef);
    if (cStage < 0 || cStage > ReferenceStates::kMaxStage)
        return;
    if (cStage == 0)
        NoteReset(aFormId);
    Send(pRef, ReferenceStates::Kind::Destruction, static_cast<uint8_t>(cStage));
    spdlog::debug("ReferenceStates: {:X} is at destruction stage {} here; sent", aFormId, cStage);
}

void ReferenceStateService::OnBearTrap(const BearTrapEvent& acEvent) noexcept
{
    if (!m_bearTraps.Played(acEvent.FormId, acEvent.Armed, acEvent.Load, std::chrono::steady_clock::now()) || !m_transport.IsConnected())
        return;
    if (const TESObjectREFR* pRef = GetRef(acEvent.FormId); pRef && IsAttachedRef(pRef))
    {
        Send(pRef, ReferenceStates::Kind::Trap, static_cast<uint8_t>(acEvent.Armed ? 1 : 0));
        spdlog::debug("ReferenceStates: bear trap {:X} {} here; sent", acEvent.FormId, acEvent.Armed ? "armed" : "sprung");
    }
}

void ReferenceStateService::NoteReset(uint32_t aFormId) noexcept
{
    m_resets.Note(aFormId, std::chrono::steady_clock::now());
    m_stages.Cancel(aFormId);
    m_pending.Cancel(aFormId);
}

// This player's own pick of a plant: sent once the game shows it picked.
void ReferenceStateService::OnActivate(const ActivateEvent& acEvent) noexcept
{
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || acEvent.ActivatorId != pPlayer->formID || !m_transport.IsConnected())
        return;
    const TESObjectREFR* pRef = GetRef(acEvent.ObjectId);
    if (!pRef || pRef->IsTemporary() || !pRef->baseForm)
        return;
    if (IsPlant(pRef) || ReferenceStates::IsNirnroot(pRef->baseForm->formID))
        m_uses.Used(acEvent.ObjectId, std::chrono::steady_clock::now());
}

void ReferenceStateService::Send(const TESObjectREFR* apRef, ReferenceStates::Kind aKind, uint8_t aValue) noexcept
{
    const TESObjectCELL* pCell = apRef->GetParentCellEx();
    auto& modSystem = m_world.GetModSystem();
    RequestReferenceState request;
    if (!pCell || !modSystem.GetServerModId(apRef->formID, request.State.Id) || !modSystem.GetServerModId(pCell->formID, request.CellId))
        return;
    if (const TESWorldSpace* pWorldSpace = apRef->GetWorldSpace())
    {
        if (!modSystem.GetServerModId(pWorldSpace->formID, request.WorldSpaceId))
            return;
        request.CenterCoords = GridCellCoords::CalculateGridCellCoords(apRef->position.x, apRef->position.y);
    }
    request.Position = glm::vec3(apRef->position.x, apRef->position.y, apRef->position.z);
    request.State.Kind = static_cast<uint8_t>(aKind);
    request.State.Value = aValue;
    if (!m_outbox.Push(std::move(request)))
        spdlog::debug("ReferenceStates: more reports waiting than {}; the oldest is dropped", ReferenceStates::SendPacer<RequestReferenceState>::kMaxWaiting);
}

void ReferenceStateService::OnNotifyReferenceStates(const NotifyReferenceStates& acMessage) noexcept
{
    const auto cNow = std::chrono::steady_clock::now();
    for (const ReferenceState& cState : acMessage.States)
    {
        // A kind a later version added is not this game's to apply.
        if (!ReferenceStates::IsKnown(cState.Kind) || !ReferenceStates::IsValid(cState.Kind, cState.Value))
            continue;
        // Kept from before this player came: only while this game would not have reset it by now.
        if (!ReferenceStates::IsFresh(cState.IdleHours, ReferenceStates::kDefaultRespawnHours))
        {
            spdlog::debug(
                "ReferenceStates: {:X}:{:X} kind {} kept {} game hours unloaded, past this game's respawn time; left as it is", cState.Id.ModId, cState.Id.BaseId,
                static_cast<int>(cState.Kind), cState.IdleHours);
            continue;
        }
        const uint32_t cFormId = m_world.GetModSystem().GetGameId(cState.Id);
        if (!cFormId)
            continue;
        // Not forced over this game's own reset of it just now.
        if (m_resets.IsRecent(cFormId, cNow))
            continue;

        switch (static_cast<ReferenceStates::Kind>(cState.Kind))
        {
        case ReferenceStates::Kind::Destruction:
            if (cState.Value > 0)
                m_stages.Add(cFormId, cState.Value, cNow);
            break;
        case ReferenceStates::Kind::Trap: m_bearTraps.Want(cFormId, cState.Value != 0, cNow); break;
        case ReferenceStates::Kind::Used:
            if (cState.Value > 0)
                m_pending.Add(cFormId, cState.Value, cNow);
            break;
        }
    }

    spdlog::debug(
        "ReferenceStates: {} state(s) from other games; {} stage(s), {} use(s) and {} trap(s) waiting", acMessage.States.size(), m_stages.Size(), m_pending.Size(),
        m_bearTraps.WantedCount());
}

void ReferenceStateService::OnUpdate(const UpdateEvent&) noexcept
{
    // A loaded save has its own plants, veins and traps: a use watched before it would read them as back as new and end the
    // party's states, and a report or a settle from before it is about a world that is gone.
    // The server is told after the trackers are cleared, so the states it sends again are kept.
    if (m_saveLoaded.exchange(false))
    {
        ForgetTracked();
        spdlog::debug("ReferenceStates: a save was loaded; what this game tracked before it is forgotten");
        if (m_transport.IsConnected())
            m_transport.Send(RequestSaveLoaded{});
    }

    if (!m_transport.IsConnected())
        return;

    const auto cNow = std::chrono::steady_clock::now();
    m_outbox.Drain(cNow, [this](const RequestReferenceState& acRequest) { m_transport.Send(acRequest); });
    if (!IsPlaying())
        return;

    // Another game's bear trap state: this one is used until it matches. Used by itself, as its script ignores who uses it,
    // and an armed trap closes without a hit.
    const auto cTrapLoaded = [](uint32_t aFormId)
    {
        TESObjectREFR* pRef = GetRef(aFormId);
        return pRef && IsAttachedRef(pRef) && ReferenceStates::IsBearTrap(pRef->baseForm->formID) && pRef->GetNiNode();
    };
    while (const uint32_t cTrapId = m_bearTraps.Next(cNow, cTrapLoaded))
    {
        if (TESObjectREFR* pTrap = GetRef(cTrapId))
        {
            spdlog::debug("ReferenceStates: bear trap {:X} used to match another game's", cTrapId);
            pTrap->Activate(pTrap, 0, nullptr, 1, false);
        }
    }

    if (m_stages.Size() > 0)
    {
        PAPYRUS_FUNCTION(void, ObjectReference, DamageObject, float);
        PAPYRUS_FUNCTION(void, ObjectReference, SetDestroyed, bool);

        const auto cStageOf = [this](uint32_t aFormId) -> std::optional<int32_t>
        {
            TESObjectREFR* pRef = GetRef(aFormId);
            if (!pRef || !IsAttachedRef(pRef) || !pRef->GetNiNode())
                return std::nullopt;
            // Only those whose stage runs no script and hurts no one here; never an actor.
            if (pRef->formType == FormType::Character || !IsSharedDestructible(pRef))
                return -1;
            return CurrentStage(pRef);
        };
        const auto cDamage = [this, cNow](uint32_t aFormId, float aDamage)
        {
            m_settled[aFormId] = cNow;
            if (const TESObjectREFR* pRef = GetRef(aFormId))
                s_pDamageObject(pRef, aDamage);
        };
        const auto cReached = [this, cNow](uint32_t aFormId)
        {
            m_settled[aFormId] = cNow;
            TESObjectREFR* pRef = GetRef(aFormId);
            if (!pRef)
                return;
            // MineOreScript sets a depleted vein destroyed after damaging it: it is mined no more until its cell resets.
            if (IsOreVein(pRef))
                s_pSetDestroyed(pRef, true);
            spdlog::debug("ReferenceStates: {:X} at another game's destruction stage", aFormId);
        };
        m_stages.Update(cNow, cStageOf, cDamage, cReached);
    }

    const auto cReady = m_pending.Ready(
        cNow,
        [](uint32_t aFormId)
        {
            TESObjectREFR* pRef = GetRef(aFormId);
            return pRef && IsAttachedRef(pRef) && pRef->GetNiNode();
        });
    for (const auto& cEntry : cReady)
        ApplyUsed(cEntry.first);

    if (cNow < m_nextWatch)
        return;
    m_nextWatch = cNow + kRuneWatchInterval;

    std::erase_if(m_settled, [cNow](const auto& acEntry) { return cNow - acEntry.second >= kSettledWindow; });
    m_resets.Forget(cNow);
    WatchRunes();
    m_uses.Update(
        cNow, [this](uint32_t aFormId) { return LookOf(aFormId); },
        [this](uint32_t aFormId, bool aUsed)
        {
            if (!aUsed)
                NoteReset(aFormId);
            if (const TESObjectREFR* pRef = GetRef(aFormId))
            {
                Send(pRef, ReferenceStates::Kind::Used, static_cast<uint8_t>(aUsed ? 1 : 0));
                spdlog::debug("ReferenceStates: {:X} {} here; sent", aFormId, aUsed ? "used up" : "back as new");
            }
        });
}

// Another game's plant picked or rune gone off: the same here, without an item for anyone.
void ReferenceStateService::ApplyUsed(uint32_t aFormId) noexcept
{
    TESObjectREFR* pRef = GetRef(aFormId);
    if (!pRef || !pRef->baseForm || LookOf(aFormId) != ReferenceStates::Look::New)
        return;

    const auto cNow = std::chrono::steady_clock::now();
    const uint32_t cBaseId = pRef->baseForm->formID;
    if (ReferenceStates::IsNirnroot(cBaseId) || ReferenceStates::IsRuneProjectile(cBaseId))
    {
        // Disabled, as a picked nirnroot and a rune gone off are. A nirnroot's own script would add to this player's
        // "Nirnroots Found" and give the item to whoever it names; its glow stays until the cell loads again, and the picked
        // stalk is not shown.
        ScopedReferenceEnableOverride _;
        pRef->Disable();
    }
    else if (IsPlant(pRef))
    {
        pRef->flags |= kHarvestedFlag;
        pRef->MarkChanged(kChangeEmpty);
        // The model shows a picked plant as it loads, so it is loaded again.
        if (pRef->GetNiNode())
        {
            ScopedReferenceEnableOverride _;
            pRef->Disable();
            pRef->EnableImpl();
        }
    }
    else
    {
        return;
    }

    m_uses.Applied(aFormId, cNow);
    spdlog::debug("ReferenceStates: {:X} used up in another game; the same here", aFormId);
}

// The rune traps in the cells loaded around this player, watched for the one that goes off.
void ReferenceStateService::WatchRunes() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    TESObjectCELL* pCell = pPlayer ? pPlayer->GetParentCellEx() : nullptr;
    if (!pCell)
        return;

    Vector<TESObjectCELL*> cells{pCell};
    const TES* pTES = TES::Get();
    if (TESWorldSpace* pWorldSpace = pPlayer->GetWorldSpace(); pWorldSpace && pTES)
    {
        for (const ChestSelection::Cell& cGrid : ChestSelection::CellsAround(pTES->currentGridX, pTES->currentGridY, 1))
        {
            TESObjectCELL* pAround = ModManager::Get()->GetCellFromCoordinates(cGrid.X, cGrid.Y, pWorldSpace, false);
            if (pAround && pAround != pCell && pAround->IsAttached())
                cells.push_back(pAround);
        }
    }

    static const Vector<FormType> s_types{FormType::Projectile};
    for (const TESObjectCELL* pObjectsCell : cells)
    {
        for (const TESObjectREFR* pRef : pObjectsCell->GetRefsByFormTypes(s_types))
        {
            if (pRef && pRef->baseForm && !pRef->IsTemporary() && ReferenceStates::IsRuneProjectile(pRef->baseForm->formID) && !m_uses.IsWatched(pRef->formID) &&
                LookOf(pRef->formID) == ReferenceStates::Look::New)
                m_uses.Armed(pRef->formID);
        }
    }
}

ReferenceStates::Look ReferenceStateService::LookOf(uint32_t aFormId) noexcept
{
    const TESObjectREFR* pRef = GetRef(aFormId);
    if (!pRef || !pRef->baseForm)
        return ReferenceStates::Look::Unloaded;
    const TESObjectCELL* pCell = pRef->GetParentCellEx();
    if (!pCell || !pCell->IsAttached())
        return ReferenceStates::Look::Unloaded;

    const uint32_t cBaseId = pRef->baseForm->formID;
    if (ReferenceStates::IsNirnroot(cBaseId) || ReferenceStates::IsRuneProjectile(cBaseId))
        return pRef->IsDisabled() || pRef->IsDeleted() ? ReferenceStates::Look::Used : ReferenceStates::Look::New;
    if (IsPlant(pRef))
        return (pRef->flags & kHarvestedFlag) != 0 ? ReferenceStates::Look::Used : ReferenceStates::Look::New;
    return ReferenceStates::Look::Unloaded;
}

// A destructible linked to a mining furniture: MineOreScript's vein.
bool ReferenceStateService::IsOreVein(TESObjectREFR* apRef) noexcept
{
    PAPYRUS_FUNCTION(TESObjectREFR*, ObjectReference, GetLinkedRef, BGSKeyword*);

    const TESObjectREFR* pLinked = s_pGetLinkedRef(apRef, nullptr);
    if (!pLinked || !pLinked->baseForm || static_cast<uint8_t>(pLinked->baseForm->formType) != kFurnitureType)
        return false;
    const uint32_t cBaseId = pLinked->baseForm->formID;
    return ReferenceStates::IsMiningFurniture(PluginOf(cBaseId), cBaseId & 0xFFFFFF);
}

// An ore vein, or one of the masters' destructibles whose stage runs no script and hurts no one; never one a quest holds.
bool ReferenceStateService::IsSharedDestructible(TESObjectREFR* apRef) noexcept
{
    if (!apRef->baseForm || apRef->extraData.Contains(ExtraDataType::AliasInstanceArray))
        return false;
    const uint32_t cBaseId = apRef->baseForm->formID;
    return ReferenceStates::IsPlainDestructible(PluginOf(cBaseId), cBaseId & 0xFFFFFF) || IsOreVein(apRef);
}

ActivationCatchUp::Plugin ReferenceStateService::PluginOf(uint32_t aFormId) noexcept
{
    if (!m_pluginsLooked)
    {
        m_pluginsLooked = true;
        m_dragonbornPrefix = PluginPrefix("Dragonborn.esm");
    }

    const uint32_t cPrefix = aFormId >> 24;
    if (cPrefix == 0)
        return ActivationCatchUp::Plugin::Skyrim;
    if (cPrefix == m_dragonbornPrefix)
        return ActivationCatchUp::Plugin::Dragonborn;
    return ActivationCatchUp::Plugin::Other;
}
