#include <TiltedOnlinePCH.h>

// Radiant quest alias sync (Systems/QuestAliasSync.h says what is sent and followed). Only while the server sets
// Gameplay:bEnableQuestAliasSync: otherwise nothing is read, sent or filled.

#include <Services/QuestService.h>
#include <Services/PapyrusService.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>
#include <Systems/QuestAliasReader.h>
#include <Systems/ModSystem.h>

#include <Events/PartyJoinedEvent.h>
#include <Messages/RequestQuestAliases.h>
#include <Messages/NotifyQuestAliases.h>
#include <Messages/NotifyPartyInfo.h>

#include <Forms/TESQuest.h>
#include <Games/TES.h>
#include <Interface/UI.h>

#include <optional>

namespace
{
using QuestAliasReader::AliasHeader;
using QuestAliasSync::Fill;

TESForm* GetLocation(const BGSLocAlias* apAlias) noexcept
{
    using LocationAlias = BGSLocAlias;
    PAPYRUS_FUNCTION(TESForm*, LocationAlias, GetLocation);
    return s_pGetLocation(apAlias);
}

// The quest's reference and location aliases as this game has them, targets in server ids. An empty alias, or one
// whose target has no server id (a reference made at run time), has an empty target.
// Nothing when the aliases are not laid out as expected: then nothing is sent or filled.
std::optional<std::vector<Fill>> ReadFills(World& aWorld, TESQuest* apQuest) noexcept
{
    static PapyrusLookup s_getLocation("LocationAlias", "GetLocation");
    ModSystem& modSystem = aWorld.GetModSystem();
    std::vector<Fill> fills;
    for (uint32_t i = 0; i < apQuest->aliases.length; ++i)
    {
        auto* pHeader = static_cast<AliasHeader*>(apQuest->aliases[i]);
        if (!pHeader)
            continue;
        if (pHeader->owningQuest != apQuest)
            return std::nullopt;

        auto* pBase = reinterpret_cast<BGSBaseAlias*>(pHeader);
        Fill fill{};
        fill.AliasId = static_cast<uint16_t>(pHeader->aliasID);
        if (Cast<BGSRefAlias>(pBase))
        {
            fill.AliasKind = QuestAliases::kRef;
            TESObjectREFR* pRef = apQuest->GetAliasedRef(pHeader->aliasID);
            if (pRef && !pRef->IsTemporary())
                modSystem.GetServerModId(pRef->formID, fill.Target);
        }
        else if (auto* pLocation = Cast<BGSLocAlias>(pBase))
        {
            // A native that is not registered must not read as an empty alias.
            if (!s_getLocation.Resolve())
                return std::nullopt;
            fill.AliasKind = QuestAliases::kLocation;
            if (TESForm* pForm = GetLocation(pLocation))
                modSystem.GetServerModId(pForm->formID, fill.Target);
        }
        else
            continue;
        fills.push_back(fill);
    }
    return fills;
}

void* FindAlias(TESQuest* apQuest, uint16_t aAliasId) noexcept
{
    for (uint32_t i = 0; i < apQuest->aliases.length; ++i)
    {
        auto* pHeader = static_cast<AliasHeader*>(apQuest->aliases[i]);
        if (pHeader && pHeader->owningQuest == apQuest && pHeader->aliasID == aAliasId)
            return pHeader;
    }
    return nullptr;
}

const char* QuestName(TESQuest* apQuest) noexcept
{
    const char* cpName = apQuest->idName.AsAscii();
    return cpName ? cpName : "";
}

bool IsRunningHere(const TESQuest* apQuest) noexcept
{
    return apQuest && apQuest->IsEnabled() && !apQuest->IsStopped();
}

// A load replaces the quests and their aliases: nothing is read or forced until it is over (as the war does).
bool IsQuestStateReadable() noexcept
{
    static const BSFixedString s_loadingMenu("Loading Menu");
    static const BSFixedString s_mainMenu("Main Menu");
    const UI* pUI = UI::Get();
    return pUI && !pUI->GetMenuOpen(s_loadingMenu) && !pUI->GetMenuOpen(s_mainMenu);
}

bool IsScenePlaying(TESQuest* apQuest) noexcept
{
    for (BGSScene* pScene : apQuest->scenes)
        if (pScene && pScene->isPlaying)
            return true;
    return false;
}

// A target resolved in this game, or null while it is not: missing, not loaded, or of the wrong kind for the alias.
struct ResolvedFill
{
    void* pAlias;
    TESForm* pTarget;
};

std::optional<ResolvedFill> Resolve(World& aWorld, TESQuest* apQuest, const Fill& acFill) noexcept
{
    void* pAlias = FindAlias(apQuest, acFill.AliasId);
    TESForm* pTarget = TESForm::GetById(aWorld.GetModSystem().GetGameId(acFill.Target));
    if (!pAlias || !pTarget)
        return std::nullopt;

    auto* pBase = static_cast<BGSBaseAlias*>(pAlias);
    if (acFill.AliasKind == QuestAliases::kLocation)
    {
        if (!Cast<BGSLocAlias>(pBase) || !Cast<BGSLocation>(pTarget))
            return std::nullopt;
    }
    else if (!Cast<BGSRefAlias>(pBase) || !Cast<TESObjectREFR>(pTarget))
        return std::nullopt;

    return ResolvedFill{pAlias, pTarget};
}
} // namespace

bool QuestService::IsAliasSyncOn() const noexcept
{
    return m_world.GetServerSettings().QuestAliasSync && m_world.GetTransport().IsConnected() && m_world.GetPartyService().IsInParty();
}

bool QuestService::IsAliasSyncQuest(TESQuest* apQuest) noexcept
{
    return apQuest && apQuest->aliases.length > 0 && !IsNonSyncableQuest(apQuest) && QuestAliasSync::IsRadiantQuest(QuestName(apQuest));
}

void QuestService::SyncAliases(uint32_t aQuestFormId, AliasEvent aEvent) noexcept
{
    if (aEvent == AliasEvent::Stopped)
    {
        m_aliasSent.Forget(aQuestFormId);
        m_aliasWanted.Forget(aQuestFormId);
        return;
    }

    if (!IsAliasSyncOn())
        return;

    // A start of this game's own is a new run that this game may pick for: fills of an earlier run are not forced on it.
    if (aEvent == AliasEvent::LocalStart)
        m_aliasWanted.Forget(aQuestFormId);

    // Following another game's picks: they may now be applied, if the quest has just started here.
    if (m_aliasWanted.Find(aQuestFormId))
    {
        ApplyWantedAliases(aQuestFormId);
        return;
    }

    // A start or stage applied from another player is theirs to pick: whoever claims it sends.
    // A stage's fills go out at the next check, after the start's and after the stage's fragment ran.
    if (aEvent == AliasEvent::LocalStart)
        SendAliases(aQuestFormId);
    else if (aEvent == AliasEvent::Local && IsAliasSyncQuest(Cast<TESQuest>(TESForm::GetById(aQuestFormId))))
        m_aliasSent.Offer(aQuestFormId);
}

void QuestService::SendAliases(uint32_t aQuestFormId) noexcept
{
    TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(aQuestFormId));
    if (!IsAliasSyncOn() || !IsQuestStateReadable() || !IsAliasSyncQuest(pQuest) || !IsRunningHere(pQuest) || m_aliasWanted.Find(aQuestFormId))
        return;

    GameId questId{};
    if (!m_world.GetModSystem().GetServerModId(aQuestFormId, questId))
        return;
    // Checked again every pass from now on: aliases may still be filling.
    m_aliasSent.Offer(aQuestFormId);
    const auto cFills = ReadFills(m_world, pQuest);
    if (!cFills)
        return;

    RequestQuestAliases request{};
    request.Aliases.Quest = questId;
    for (const Fill& cFill : *cFills)
        if (cFill.Target)
            request.Aliases.Fills.push_back(cFill);
    if (request.Aliases.Fills.empty() || !m_aliasSent.ShouldSend(aQuestFormId, QuestAliasSync::Hash(request.Aliases.Fills), std::chrono::steady_clock::now()))
        return;

    spdlog::debug("QuestAliasSync: sent the {} alias fills of {} ({:X})", request.Aliases.Fills.size(), QuestName(pQuest), aQuestFormId);
    m_world.GetTransport().Send(request);
}

void QuestService::SendRunningQuestAliases(const char* acpWhy) noexcept
{
    // The leader's picks stand for quests every game already runs (two saves loaded): the others follow them.
    if (!IsAliasSyncOn() || !m_world.GetPartyService().IsLeader())
        return;

    size_t count = 0;
    for (TESQuest* pQuest : ModManager::Get()->quests)
    {
        if (IsAliasSyncQuest(pQuest) && IsRunningHere(pQuest) && !m_aliasWanted.Find(pQuest->formID))
        {
            SendAliases(pQuest->formID);
            ++count;
        }
    }
    if (count)
        spdlog::debug("QuestAliasSync: checking the alias fills of {} running radiant quests at {}", count, acpWhy);
}

void QuestService::OnQuestAliases(const NotifyQuestAliases& acMessage) noexcept
{
    if (!m_world.GetServerSettings().QuestAliasSync)
        return;

    const uint32_t cQuestFormId = m_world.GetModSystem().GetGameId(acMessage.Aliases.Quest);
    TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(cQuestFormId));
    if (!IsAliasSyncQuest(pQuest))
    {
        spdlog::debug("QuestAliasSync: fills for quest {:X} not taken: no such radiant quest here", acMessage.Aliases.Quest.LogFormat());
        return;
    }

    // A fill with no target or an unknown kind could never be applied, and would hold back the rest.
    std::vector<Fill> fills;
    for (const Fill& cFill : acMessage.Aliases.Fills)
        if (QuestAliasSync::IsUsable(cFill))
            fills.push_back(cFill);
    if (fills.empty())
        return;

    // Another game picks this run's targets: this one stops offering its own.
    m_aliasSent.Forget(cQuestFormId);
    m_aliasWanted.Set(cQuestFormId, std::move(fills), std::chrono::steady_clock::now());
    ApplyWantedAliases(cQuestFormId);
}

void QuestService::ApplyWantedAliases(uint32_t aQuestFormId) noexcept
{
    auto* pEntry = m_aliasWanted.Find(aQuestFormId);
    TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(aQuestFormId));
    if (!pEntry || !pEntry->Pending || !pQuest || !IsQuestStateReadable())
        return;

    const auto logWait = [&](const char* acpWhy)
    {
        if (!pEntry->WaitLogged)
            spdlog::debug("QuestAliasSync: the other player's targets for {} ({:X}) wait: {}", QuestName(pQuest), aQuestFormId, acpWhy);
        pEntry->WaitLogged = true;
    };

    if (!IsRunningHere(pQuest))
        return logWait("the quest does not run here yet");
    pEntry->SeenRunning = true;

    // Moving a scene's actor mid-phase can leave it stuck, as a stage set under it can (QuestHold).
    if (IsScenePlaying(pQuest) || m_heldStages.count(aQuestFormId) != 0)
        return logWait("a scene of the quest is playing");

    const auto cLocal = ReadFills(m_world, pQuest);
    if (!cLocal)
    {
        spdlog::warn("QuestAliasSync: the aliases of {} ({:X}) cannot be read here; the other player's targets are not taken", QuestName(pQuest), aQuestFormId);
        m_aliasWanted.Forget(aQuestFormId);
        return;
    }

    const auto cDifferences = QuestAliasSync::Differences(pEntry->Fills, *cLocal);

    // All or none: a camp from one game and a chief from the other would point the journal at neither.
    Vector<ResolvedFill> resolved;
    for (const Fill& cFill : cDifferences)
    {
        const auto cResolved = Resolve(m_world, pQuest, cFill);
        if (!cResolved)
            return logWait("a target is not found or not loaded here; this game keeps its own picks until it is");
        resolved.push_back(*cResolved);
    }

    // A native missing on this setup would make every call below do nothing.
    static PapyrusLookup s_forceRef("ReferenceAlias", "ForceRefTo");
    static PapyrusLookup s_forceLocation("LocationAlias", "ForceLocationTo");
    if (!s_forceRef.Resolve() || !s_forceLocation.Resolve())
        return logWait("the game's ForceRefTo or ForceLocationTo is not registered");

    using ReferenceAlias = BGSRefAlias;
    using LocationAlias = BGSLocAlias;
    PAPYRUS_FUNCTION(void, ReferenceAlias, ForceRefTo, TESObjectREFR*);
    PAPYRUS_FUNCTION(void, LocationAlias, ForceLocationTo, TESForm*);

    // In Differences' order: locations first.
    String applied;
    for (size_t i = 0; i < resolved.size(); ++i)
    {
        auto* pBase = static_cast<BGSBaseAlias*>(resolved[i].pAlias);
        if (cDifferences[i].AliasKind == QuestAliases::kLocation)
            s_pForceLocationTo(Cast<BGSLocAlias>(pBase), resolved[i].pTarget);
        else
            s_pForceRefTo(Cast<BGSRefAlias>(pBase), Cast<TESObjectREFR>(resolved[i].pTarget));
        applied += spdlog::fmt_lib::format(" {}={:X}", cDifferences[i].AliasId, resolved[i].pTarget->formID);
    }

    pEntry->Pending = false;
    if (!applied.empty())
        spdlog::debug("QuestAliasSync: {} ({:X}) takes the other player's targets, alias=form:{}", QuestName(pQuest), aQuestFormId, applied);
}

void QuestService::UpdateAliasSync() noexcept
{
    constexpr auto cPassEvery = 2s;
    if (m_aliasWanted.Empty() && m_aliasSent.Empty())
        return;
    const auto cNow = std::chrono::steady_clock::now();
    if (cNow < m_nextAliasPass || !IsQuestStateReadable())
        return;
    m_nextAliasPass = cNow + cPassEvery;

    for (const uint32_t cQuestFormId : m_aliasWanted.TakeExpired(cNow))
        spdlog::debug("QuestAliasSync: gave up on the other player's targets for quest {:X} after 10 minutes unapplied; this game keeps its own for this run", cQuestFormId);

    // A quest stopped from another player's update fires no stop event here: a stopped quest ends the run.
    for (const uint32_t cQuestFormId : m_aliasWanted.Quests())
    {
        auto* pEntry = m_aliasWanted.Find(cQuestFormId);
        if (!IsRunningHere(Cast<TESQuest>(TESForm::GetById(cQuestFormId))))
        {
            if (pEntry->SeenRunning)
                m_aliasWanted.Forget(cQuestFormId);
            continue;
        }
        ApplyWantedAliases(cQuestFormId);
    }

    // The picking game checks its fills again: a stage's fragment, here or relayed, may have re-picked after its event.
    for (const uint32_t cQuestFormId : m_aliasSent.Quests())
    {
        if (!IsRunningHere(Cast<TESQuest>(TESForm::GetById(cQuestFormId))))
            m_aliasSent.Forget(cQuestFormId);
        else
            SendAliases(cQuestFormId);
    }
}

void QuestService::OnPartyInfoForAliases(const NotifyPartyInfo& acMessage) noexcept
{
    if (!m_world.GetServerSettings().QuestAliasSync)
        return;

    // Someone joined: whatever this game picked goes out again for them. A new leader offers what runs in its save.
    const bool cGrew = acMessage.PlayerIds.size() > m_aliasPartySize;
    const bool cBecameLeader = acMessage.IsLeader && !m_aliasWasLeader;
    m_aliasPartySize = acMessage.PlayerIds.size();
    m_aliasWasLeader = acMessage.IsLeader;
    if (cGrew)
        m_aliasSent.Reoffer();
    if (cGrew || cBecameLeader)
        m_world.GetRunner().Queue([this]() { SendRunningQuestAliases("a party change"); });
}

void QuestService::OnPartyJoinedForAliases(const PartyJoinedEvent&) noexcept
{
    if (m_world.GetServerSettings().QuestAliasSync)
        m_world.GetRunner().Queue([this]() { SendRunningQuestAliases("joining the party"); });
}

void QuestService::ForgetAliasSync() noexcept
{
    m_aliasSent.Clear();
    m_aliasWanted.Clear();
    m_aliasPartySize = 0;
    m_aliasWasLeader = false;
}
