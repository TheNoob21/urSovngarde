#include <GameServer.h>
#include <Components.h>

#include <World.h>
#include <Services/QuestService.h>

#include <Messages/RequestQuestUpdate.h>
#include <Messages/NotifyQuestUpdate.h>
#include <Messages/RequestWarState.h>
#include <Messages/NotifyWarState.h>
#include <Messages/RequestTrackedQuests.h>
#include <Messages/NotifyTrackedQuests.h>
#include <Messages/RequestWordLearned.h>
#include <Messages/NotifyWordLearned.h>
#include <Messages/RequestQuestCatchUp.h>
#include <Messages/NotifyQuestCatchUp.h>

#include <Setting.h>
#include <SharedHomes.h>
namespace
{
static_assert(PartyWords::kMaxPerParty == NotifyWordLearned::kMax, "a party's words go to a joining player in one message");

Console::Setting bEnableMiscQuestSync{"Gameplay:bEnableMiscQuestSync", "(Experimental) Syncs miscellaneous quests when possible", true};
Console::Setting bEnableQuestCatchUp{
    "Gameplay:bEnableQuestCatchUp", "(Experimental) A player who joins a party later, or comes back after losing the connection, catches up on the quest stages the party was sent",
    true};

} // namespace

bool QuestService::IsCatchUpOn() noexcept
{
    return bEnableQuestCatchUp;
}

QuestService::QuestService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_questUpdateConnection = aDispatcher.sink<PacketEvent<RequestQuestUpdate>>().connect<&QuestService::OnQuestChanges>(this);
    m_warStateConnection = aDispatcher.sink<PacketEvent<RequestWarState>>().connect<&QuestService::OnWarState>(this);
    m_trackedQuestsConnection = aDispatcher.sink<PacketEvent<RequestTrackedQuests>>().connect<&QuestService::OnTrackedQuests>(this);
    m_wordLearnedConnection = aDispatcher.sink<PacketEvent<RequestWordLearned>>().connect<&QuestService::OnWordLearned>(this);
    m_questCatchUpConnection = aDispatcher.sink<PacketEvent<RequestQuestCatchUp>>().connect<&QuestService::OnQuestCatchUpAsked>(this);
}

void QuestService::OnWarState(const PacketEvent<RequestWarState>& acMessage) noexcept
{
    // Only the leader's war is followed: a member's stays its own.
    auto& partyService = m_world.GetPartyService();
    if (!partyService.GetPlayerParty(acMessage.pPlayer) || !partyService.IsPlayerLeader(acMessage.pPlayer))
        return;

    NotifyWarState notify{};
    notify.State = acMessage.Packet.State;
    GameServer::Get()->SendToParty(notify, acMessage.pPlayer->GetParty(), acMessage.GetSender());
}

void QuestService::OnTrackedQuests(const PacketEvent<RequestTrackedQuests>& acMessage) noexcept
{
    // Only the leader's journal is followed.
    auto& partyService = m_world.GetPartyService();
    if (!partyService.GetPlayerParty(acMessage.pPlayer) || !partyService.IsPlayerLeader(acMessage.pPlayer))
        return;

    NotifyTrackedQuests notify{};
    for (const TrackedQuest& cQuest : acMessage.Packet.Quests)
    {
        // Types None or Miscellaneous are shared, their stages too, only with the misc quest setting on.
        if ((cQuest.ClientQuestType == 0 || cQuest.ClientQuestType == 6) && !bEnableMiscQuestSync)
            continue;

        notify.Quests.push_back(cQuest);
    }

    GameServer::Get()->SendToParty(notify, acMessage.pPlayer->GetParty(), acMessage.GetSender());
}

void QuestService::OnWordLearned(const PacketEvent<RequestWordLearned>& acMessage) noexcept
{
    const PartyComponent& cParty = acMessage.pPlayer->GetParty();
    if (!cParty.JoinedPartyId)
        return;

    // A quest teaches the same word in every member's game: the party is told it once.
    // The server has no game data: each game checks that it is a word of power.
    const GameId& cWord = acMessage.Packet.WordId;
    if (!m_partyWords.IsNew(*cParty.JoinedPartyId, PartyWords::Key(cWord.ModId, cWord.BaseId)))
        return;

    NotifyWordLearned notify{};
    notify.Words.push_back(cWord);
    GameServer::Get()->SendToParty(notify, cParty, acMessage.GetSender());
}

void QuestService::NoteMemberStep(uint32_t aPlayerId) noexcept
{
    const auto cNow = std::chrono::steady_clock::now();
    for (auto it = m_memberStepAt.begin(); it != m_memberStepAt.end();)
        it = cNow - it->second > std::chrono::milliseconds(SharedHomes::kSwitchAfterUseMs) ? m_memberStepAt.erase(it) : std::next(it);
    m_memberStepAt[aPlayerId] = cNow;
}

bool QuestService::SteppedRecently(const Player* apPlayer) const noexcept
{
    if (!apPlayer)
        return false;
    const auto cAt = m_memberStepAt.find(apPlayer->GetId());
    return cAt != m_memberStepAt.end() && std::chrono::steady_clock::now() - cAt->second <= std::chrono::milliseconds(SharedHomes::kSwitchAfterUseMs);
}

// A member's game loaded a save while connected: the stages the party passed since that save never ran there.
// It is sent the party's steps again, as on joining, and skips those it has already reached.
void QuestService::OnQuestCatchUpAsked(const PacketEvent<RequestQuestCatchUp>& acMessage) noexcept
{
    Player* pPlayer = acMessage.pPlayer;
    auto& partyService = m_world.GetPartyService();
    PartyService::Party* pParty = partyService.GetPlayerParty(pPlayer);
    if (!pParty)
        return;

    const auto cNow = std::chrono::steady_clock::now();
    // Player ids are never reused: an entry past the window is no longer needed.
    for (auto it = m_catchUpAskedAt.begin(); it != m_catchUpAskedAt.end();)
        it = cNow - it->second >= std::chrono::milliseconds(QuestHistory::kAskAgainAfterMs) ? m_catchUpAskedAt.erase(it) : std::next(it);
    const auto cLast = m_catchUpAskedAt.find(pPlayer->GetId());
    const int64_t cMsSince = cLast == m_catchUpAskedAt.end() ? -1 : std::chrono::duration_cast<std::chrono::milliseconds>(cNow - cLast->second).count();
    if (!QuestHistory::MayAnswerAsk(bEnableQuestCatchUp, !pParty->Quests.GetSteps().empty(), cMsSince))
    {
        spdlog::debug("QuestCatchUp: player {} asked again after a load; nothing sent (switched off, nothing kept, or asked within 1 s)", pPlayer->GetId());
        return;
    }
    m_catchUpAskedAt[pPlayer->GetId()] = cNow;

    NotifyQuestCatchUp catchUp{};
    catchUp.Steps = pParty->Quests.GetSteps();
    pPlayer->Send(catchUp);
    spdlog::info(
        "QuestCatchUp: {} quest step(s) sent again to {} {} after a load", catchUp.Steps.size(), partyService.IsPlayerLeader(pPlayer) ? "leader" : "player", pPlayer->GetId());
}

void QuestService::OnQuestChanges(const PacketEvent<RequestQuestUpdate>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;

    auto* pPlayer = acMessage.pPlayer;

    auto& questComponent = pPlayer->GetQuestLogComponent();
    auto& entries = questComponent.QuestContent.Entries;

    auto questIt = std::find_if(entries.begin(), entries.end(), [&message](const auto& e) { return e.Id == message.Id; });
    // Whether this player's log had the quest before this step: a stage of a quest it never started does not open its switch window.
    const bool cKnownBefore = questIt != entries.end();

    NotifyQuestUpdate notify{};
    notify.Id = message.Id;
    notify.Stage = message.Stage;
    notify.Status = message.Status;
    notify.ClientQuestType = message.ClientQuestType;

    if (notify.ClientQuestType == 0 ||  notify.ClientQuestType == 6) // Types None or Miscellaneous. Hard-coded to avoid client header file.
    {
        if (!bEnableMiscQuestSync)
            return;
        spdlog::debug("{}: syncing type none/misc quest to party, gameId {:X} questStage {} questStatus {} questType {}",
                     __FUNCTION__, notify.Id.LogFormat(), notify.Stage, notify.Status, notify.ClientQuestType);
    }

    if (message.Status == RequestQuestUpdate::Started || message.Status == RequestQuestUpdate::StageUpdate)
    {
        // in order to prevent bugs when a quest is in progress
        // and being updated we add it as a new quest record to
        // maintain a proper remote questlog state.
        if (questIt == entries.end())
        {
            auto& newQuest = entries.emplace_back();
            newQuest.Id = message.Id;
            newQuest.Stage = message.Stage;

            if (message.Status == RequestQuestUpdate::Started)
            {
                spdlog::debug("Started quest: {:X} stage: {}", message.Id.LogFormat(), message.Stage);

                notify.Status = NotifyQuestUpdate::Started;
            }
            else
            {
                notify.Status = NotifyQuestUpdate::StageUpdate;
            }
        }
        else
        {
            spdlog::debug("Updated quest: {:X}, stage: {}", message.Id.LogFormat(), message.Stage);

            auto& record = *questIt;
            record.Id = message.Id;
            record.Stage = message.Stage;

            notify.Status = NotifyQuestUpdate::StageUpdate;
        }
    }
    else if (message.Status == RequestQuestUpdate::Stopped)
    {
        spdlog::debug("Stopped quest: {:X}, stage: {}", message.Id.LogFormat(), message.Stage);

        if (questIt != entries.end())
            entries.erase(questIt);
        else
            spdlog::warn("Unable to delete quest object {:X}", message.Id.LogFormat());

        notify.Status = NotifyQuestUpdate::Stopped;
    }

    const auto& partyComponent = acMessage.pPlayer->GetParty();
    PartyService::Party* pParty = m_world.GetPartyService().GetPlayerParty(pPlayer);
    if (!pParty)
        return;

    // Party members' scripts often reach the same stage on their own, only relay it once
    // Only shortly after the first relay: a reload or a reconnect brings the stages back, and no start comes to reset them.
    // A later one is relayed again, and a game that has already reached it skips it.
    if (message.Status == RequestQuestUpdate::StageUpdate)
    {
        constexpr auto cDuplicateWindow = std::chrono::seconds(30);
        const auto cNow = std::chrono::steady_clock::now();
        auto& stages = pParty->QuestStages[message.Id];
        const auto cRelayed = stages.find(message.Stage);
        if (cRelayed != stages.end() && cNow - cRelayed->second < cDuplicateWindow)
        {
            spdlog::debug("Dropped duplicate quest stage from player {}, quest: {:X}, stage: {}", pPlayer->GetId(), message.Id.LogFormat(), message.Stage);
            return;
        }
        stages[message.Stage] = cNow;
    }
    else
    {
        // A started quest has run none of its stages yet, a stopped one may run them again when restarted
        pParty->QuestStages.erase(message.Id);
    }

    // A member's own progress: what its quest's scripts switch next is the party's too, as after a lever (ObjectService, SharedHomes::SwitchesTaken).
    // Only a step the party has not had in this run: a member behind the party would take the world back to an older stage.
    // That needs the party's kept steps (Gameplay:bEnableQuestCatchUp); without them no member step opens the window.
    const QuestStep cStep{notify.Id, notify.Stage, message.Status, notify.ClientQuestType};
    if (QuestHistory::OpensSwitchWindow(m_world.GetPartyService().IsPlayerLeader(pPlayer), bEnableQuestCatchUp, cKnownBefore, message.Status, pParty->Quests.IsNew(cStep)))
        NoteMemberStep(pPlayer->GetId());

    if (bEnableQuestCatchUp)
        // As the player sent it: a quest started again here is a new run, whatever the server still held for it.
        pParty->Quests.Record(cStep);

    GameServer::Get()->SendToParty(notify, partyComponent, acMessage.GetSender());
}
