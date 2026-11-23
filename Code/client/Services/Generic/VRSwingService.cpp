#include <TiltedOnlinePCH.h>

#include <Services/VRSwingService.h>

#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Structs/ActionEvent.h>

#include <AI/AIProcess.h>
#include <Games/ActorExtension.h>
#include <Interface/UI.h>
#include <Misc/BSFixedString.h>
#include <PlayerCharacter.h>
#include <World.h>

#include <algorithm>
#include <mutex>

namespace
{
// How long a swing waits for an attack action the game performs for it itself, as a plugin's swing does.
// The copies are shown about a tenth of a second behind, so the actions still arrive before their tick plays there.
constexpr uint64_t cOwnActionWait = 60;
// An attack action of the game's own this close to a swing was performed for that swing.
constexpr uint64_t cOwnActionWindow = 250;
// A block raised for a bash is held at least this long after the bash, and lowered at the latest after the longest.
constexpr uint64_t cBashBlockShortest = 400;
constexpr uint64_t cBashBlockLongest = 1500;

// Hits reported by an add-on, from whatever thread its physics mod reports them on; a few at most, taken each update.
std::mutex s_hitLock;
TiltedPhoques::Vector<std::pair<uint32_t, uint32_t>> s_hits;
constexpr size_t cMaxHits = 4;

// The attack the game picked for the player (the high process's attackData: its graph event and ATKD flags). Null when none.
const BGSAttackData* PickedAttack(PlayerCharacter* apPlayer) noexcept
{
    const AIProcess* pProcess = apPlayer->currentProcess;
    if (!pProcess || !pProcess->highProcess)
        return nullptr;
    return pProcess->highProcess->attackData;
}

// The player's attack data and graph are read only in a session, and not while a loading screen rebuilds the player.
bool InSession(World& aWorld) noexcept
{
    if (!aWorld.GetTransport().IsOnline())
        return false;
    static const BSFixedString s_loading("Loading Menu");
    const UI* pUI = UI::Get();
    return !pUI || !pUI->GetMenuOpen(s_loading);
}
} // namespace

VRSwingService::VRSwingService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&VRSwingService::OnUpdate>(this);
    m_actionConnection = aDispatcher.sink<ActionEvent>().connect<&VRSwingService::OnActionEvent>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&VRSwingService::OnDisconnected>(this);
}

void VRSwingService::ReportHit(const uint32_t aHand, const uint32_t aFlags) noexcept
{
    std::scoped_lock lock(s_hitLock);
    if (aHand <= 1 && s_hits.size() < cMaxHits)
        s_hits.emplace_back(aHand, aFlags);
}

void VRSwingService::Reset() noexcept
{
    {
        std::scoped_lock lock(s_hitLock);
        s_hits.clear();
    }
    m_shownAt = {};
    m_hitShownAt = {};
    m_pending = {};
    m_hasNextStep = false;
    m_ownHand = -1;
    m_ownTick = 0;
    m_sent.clear();
}

void VRSwingService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    Reset();
    // The copies that held a block raised for a bash, or held up, are gone with the session.
    m_raisedBlock = false;
    m_heldBlock = false;
}

void VRSwingService::OnActionEvent(const ActionEvent& acEvent) noexcept
{
    const auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || acEvent.ActorId != pPlayer->formID || !m_world.GetTransport().IsOnline())
        return;
    const auto cOwn = std::find(m_sent.begin(), m_sent.end(), std::make_pair(acEvent.ActionId, acEvent.Tick));
    if (cOwn != m_sent.end())
    {
        m_sent.erase(cOwn);
        return;
    }
    // A block the game raises or lowers itself is shown already. Raising a block is the left attack action too, and no swing.
    if (VRSwing::Contains(acEvent.EventName.c_str(), "blockStart"))
        m_heldBlock = true;
    else if (VRSwing::Contains(acEvent.EventName.c_str(), "blockStop"))
        m_heldBlock = false;
    const int cHand = VRSwing::HandOf(acEvent.ActionId);
    if (cHand < 0 || VRSwing::Contains(acEvent.EventName.c_str(), "block"))
        return;
    m_ownHand = cHand;
    m_ownTick = acEvent.Tick;
    m_shownAt[static_cast<size_t>(cHand)] = acEvent.Tick;
}

void VRSwingService::Send(const uint32_t aActionId, const std::string_view acEventName, const uint64_t aTick) noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    // As the game's own action would be: the state just before the attack, which is not attacking yet.
    ActionEvent action;
    action.State1 = pPlayer->actorState.flags1 & 0x0FFFFFFF;
    action.State2 = pPlayer->actorState.flags2;
    action.Type = 0;
    action.Tick = aTick;
    action.ActorId = pPlayer->formID;
    action.ActionId = aActionId;
    action.TargetId = 0;
    const TiltedPhoques::String cEventName(acEventName.data(), acEventName.size());
    action.EventName = cEventName;
    pPlayer->SaveAnimationVariables(action.Variables);

    if (m_sent.size() > 16)
        m_sent.erase(m_sent.begin());
    m_sent.emplace_back(aActionId, aTick);
    m_world.GetRunner().Trigger(action);
}

void VRSwingService::OnUpdate(const UpdateEvent&) noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
    {
        m_pending = {};
        m_hasNextStep = false;
        m_raisedBlock = false;
        m_heldBlock = false;
        return;
    }

    const uint32_t cState = pPlayer->actorState.AttackState();
    const uint32_t cLoads = UI::LoadingScreensQueued();
    if (!VRSwing::KeepsPending(InSession(m_world), m_loads, cLoads))
    {
        // A swing already under way when the session starts or the load ends is not taken for a new one. A block already
        // sent for a bash stays up through a load and comes down as usual after it.
        Reset();
        if (!m_world.GetTransport().IsOnline())
            m_raisedBlock = false;
        // The other games rebuild the copy with no guard up: a block still held is raised again after the load.
        m_heldBlock = false;
        m_loads = cLoads;
        m_lastState = cState;
        return;
    }

    const uint64_t cNow = m_world.GetTick();

    bool sentBash = false;
    if (m_hasNextStep)
    {
        m_hasNextStep = false;
        Send(m_nextStep.ActionId, m_nextStep.EventName, std::max(cNow, m_pending.Tick + 1));
        m_pending = {};
        m_raisedAt = cNow;
        sentBash = true;
    }

    // The bash is over: the block raised for it comes down. The clock can step back on a resync; that counts as no time.
    const uint64_t cHeld = cNow >= m_raisedAt ? cNow - m_raisedAt : 0;
    if (m_raisedBlock && !sentBash && m_pending.Kind == VRSwing::Kind::None && cHeld >= cBashBlockShortest && (cState != VRSwing::kStateBash || cHeld > cBashBlockLongest))
    {
        m_raisedBlock = false;
        Send(VRSwing::kReleaseStep.ActionId, VRSwing::kReleaseStep.EventName, cNow);
    }

    static BSFixedString s_isBlocking("IsBlocking");
    bool blocking = false;
    pPlayer->animationGraphHolder.GetVariableBool(&s_isBlocking, &blocking);

    // A block held up shows as the guard a flat player's game raises for it, not only as the arm's pose.
    switch (VRSwing::HeldBlock(blocking, m_heldBlock, m_raisedBlock))
    {
    case VRSwing::BlockChange::Raise:
        Send(VRSwing::kRaiseStep.ActionId, VRSwing::kRaiseStep.EventName, cNow);
        m_heldBlock = true;
        break;
    case VRSwing::BlockChange::Lower:
        Send(VRSwing::kReleaseStep.ActionId, VRSwing::kReleaseStep.EventName, cNow);
        m_heldBlock = false;
        break;
    default: break;
    }

    if (VRSwing::Starts(m_lastState, cState) && pPlayer->actorState.IsWeaponDrawn())
    {
        uint32_t flags = 0;
        std::string_view event{};
        const BGSAttackData* pAttack = PickedAttack(pPlayer);
        if (pAttack)
        {
            flags = pAttack->flags;
            const char* cpEvent = pAttack->event.AsAscii();
            event = cpEvent ? std::string_view(cpEvent) : std::string_view{};
        }
        m_pending.Kind = VRSwing::Classify(cState, pAttack != nullptr, flags, event);
        m_pending.Tick = cNow;
        m_pending.FromHit = false;
        m_pending.Event.assign(event.data(), event.size());
        m_pending.Blocking = blocking || m_raisedBlock;
        spdlog::debug(
            "VRSwing: {} started (attack state {}, attack '{}', flags {:#x}, blocking {})", VRSwing::NameOf(m_pending.Kind), cState, m_pending.Event.c_str(), flags,
            m_pending.Blocking);
        // A hit showed this swing already, before the game marked it.
        if (!VRSwing::HitNeedsSwing(m_pending.Kind, m_hitShownAt, cNow))
            m_pending = {};
    }
    // Diagnostic for the crossbow reload (take out once the reload is sent): which bow and crossbow steps the VR game marks,
    // so that a reload can be sent as a flat player's game performs it. Throttled: at most 60 lines a session.
    if (VRSwing::IsBowStep(m_lastState, cState) && m_bowStepsLogged < 60)
    {
        ++m_bowStepsLogged;
        const TESForm* pRight = pPlayer->GetEquippedWeapon(1);
        spdlog::info(
            "VRBow (diagnostic): attack state {} -> {} (right hand {:X}, drawn {})", m_lastState, cState, pRight ? pRight->formID : 0, pPlayer->actorState.IsWeaponDrawn());
    }
    m_lastState = cState;

    // A hit a physics mod landed with no swing marked: shown as the swing it was, unless one for its hand was shown just before.
    TiltedPhoques::Vector<std::pair<uint32_t, uint32_t>> hits;
    {
        std::scoped_lock lock(s_hitLock);
        hits.swap(s_hits);
    }
    for (const auto& [cHand, cFlags] : hits)
    {
        const VRSwing::Kind cKind = VRSwing::FromHit(static_cast<int>(cHand), cFlags);
        if (m_pending.Kind != VRSwing::Kind::None || m_hasNextStep || !pPlayer->actorState.IsWeaponDrawn() || !VRSwing::HitNeedsSwing(cKind, m_shownAt, cNow))
            continue;
        m_pending.Kind = cKind;
        m_pending.Tick = cNow;
        m_pending.Event.clear();
        m_pending.Blocking = blocking || m_raisedBlock;
        m_pending.FromHit = true;
        spdlog::debug("VRSwing: {} from a hit (flags {:#x})", VRSwing::NameOf(cKind), cFlags);
    }

    if (m_pending.Kind == VRSwing::Kind::None || m_hasNextStep || cNow < m_pending.Tick + cOwnActionWait)
        return;

    const int cSentHand = m_ownTick + cOwnActionWindow >= m_pending.Tick && m_ownTick <= m_pending.Tick + cOwnActionWindow ? m_ownHand : -1;
    if (!VRSwing::NeedsSending(m_pending.Kind, cSentHand))
    {
        spdlog::debug("VRSwing: {} already sent by the game's own attack action", VRSwing::NameOf(m_pending.Kind));
        m_pending = {};
        return;
    }

    // Only a bash names its own steps; a plain or power attack is named after the attack the game picked.
    const std::string_view cEvent = VRSwing::IsBash(m_pending.Kind) ? std::string_view{} : std::string_view(m_pending.Event.c_str());
    const VRSwing::Steps cSteps = VRSwing::StepsFor(m_pending.Kind, m_pending.Blocking, cEvent);
    if (cSteps.Count == 0)
    {
        m_pending = {};
        return;
    }
    Send(cSteps.Items[0].ActionId, cSteps.Items[0].EventName, m_pending.Tick);
    if (const int cShownHand = VRSwing::HandOf(m_pending.Kind); cShownHand >= 0)
    {
        m_shownAt[static_cast<size_t>(cShownHand)] = m_pending.Tick;
        if (m_pending.FromHit)
            m_hitShownAt[static_cast<size_t>(cShownHand)] = m_pending.Tick;
    }
    if (cSteps.RaisedBlock)
        m_raisedBlock = true;
    // Every bash holds a block raised for it from the bash on.
    if (VRSwing::IsBash(m_pending.Kind))
        m_raisedAt = cNow;
    if (cSteps.Count > 1)
    {
        m_nextStep = cSteps.Items[1];
        m_hasNextStep = true;
        return;
    }
    m_pending = {};
}
