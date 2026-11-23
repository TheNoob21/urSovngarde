#pragma once

#include <World.h>
#include <Events/EventDispatcher.h>
#include <Games/Events.h>
#include <Structs/WarState.h>
#include <Systems/QuestAliasSync.h>
#include <Structs/QuestStep.h>
#include <Structs/TrackedQuest.h>
#include <PartyWords.h>

struct NotifyQuestUpdate;
struct NotifyQuestCatchUp;
struct NotifyWarState;
struct NotifyTrackedQuests;
struct NotifyWordLearned;
struct WordLearnedEvent;
struct NotifyQuestAliases;
struct NotifyPartyInfo;
struct PartyJoinedEvent;
struct DisconnectedEvent;
struct PartyLeftEvent;
struct UpdateEvent;

struct TESQuest;

/**
 * @brief Syncs quest starts, stops and stages across the party
 *
 * Every client runs the party's quests itself (scripts, scenes, dialogue), and only starts, stops and stages are
 * shared. Anything else a quest holds (aliases, script properties) only matches when the stage fragments produce it.
 *
 * Sending: TESQuestStartStopEvent and TESQuestStageEvent go out as RequestQuestUpdate, only in a party. Failed starts,
 * quests without stages and those in kNonSyncableQuestIds are never sent, nor None/Miscellaneous ones that stay each
 * player's own (QuestFamilies), and the server drops None/Miscellaneous ones unless bEnableMiscQuestSync is set.
 *
 * Relaying: the server sends updates to the rest of the party as NotifyQuestUpdate. Members' scripts often reach the
 * same stage on their own, so it relays each once, from whoever reaches it first, until the quest next starts or stops.
 *
 * Applying: stages go through ScriptSetStage so their fragments run (and a stopped quest restarts), skipping those
 * already reached, except that a member runs them again on a quest that allows repeated stages. Stops skip scripts.
 * ScopedQuestOverride keeps the events this fires from being sent back.
 *
 * Scenes: setting a stage a playing scene waits on can end its phase early, skipping its lines and scripts and leaving
 * the quest stuck. So the leader, whose world the party follows, holds member stages that the running or next phase checks and
 * queues later ones behind them. Members apply every stage right away.
 *
 * The civil war: its battle quests run on the leader only, so a member's game never runs the scripts that give a hold to
 * the winner and move the journal on. The leader sends who owns each hold and the state of each objective, and a member
 * sets the same on its own quest and holds.
 *
 * Radiant quests (Systems/QuestAliasSync.h), only with the server's Gameplay:bEnableQuestAliasSync: the game that started
 * one sends what its aliases were filled with, and the others fill the same aliases (QuestAliasSyncService.cpp).
 */
class QuestService final : public BSTEventSink<TESQuestStartStopEvent>,
                           BSTEventSink<TESQuestStageEvent>,
                           BSTEventSink<TESQuestStageItemDoneEvent>,
                           BSTEventSink<TESSceneEvent>,
                           BSTEventSink<TESSceneActionEvent>,
                           BSTEventSink<TESScenePhaseEvent>,
                           BSTEventSink<TESTopicInfoEvent>,
                           BSTEventSink<TESLoadGameEvent>
{
public:
    QuestService(World&, entt::dispatcher&);
    ~QuestService() = default;

    static bool IsNonSyncableQuest(TESQuest* apQuest);
    //! While the quest catch-up runs, and shortly after, this game shares no switch, the leader's either (SharedHomes::CatchUpHoldsSwitches).
    [[nodiscard]] bool CatchUpHoldsSwitches() const noexcept;
    static void DebugDumpQuests();
    static bool StopQuest(uint32_t aformId);

    struct HeldStages
    {
        // Member stages not applied yet, sorted
        Vector<uint16_t> Stages{};
        struct AppliedStage
        {
            uint16_t Stage;
            std::chrono::steady_clock::time_point At;
        };

        // Applied stages none of whose fragments has reported done yet, with when each was applied
        Vector<AppliedStage> Applied{};
        // When a stage was last held, applied or done waiting for.
        // After a long time without, the rest are applied (see FlushHeldStages).
        std::chrono::steady_clock::time_point LastProgress{};
    };

private:
    // Radiant quest alias sync, all on the game's main thread (queued on the runner from the quest events).
    enum class AliasEvent
    {
        LocalStart, // a start of this game's own
        Local,      // a stage of this game's own
        Remote,     // a start or stage applied from another player
        Stopped
    };
    bool IsAliasSyncOn() const noexcept;
    static bool IsAliasSyncQuest(TESQuest* apQuest) noexcept;
    // After a quest event: sends this game's fills of its own start or stage, or applies another game's once the quest runs.
    void SyncAliases(uint32_t aQuestFormId, AliasEvent aEvent) noexcept;
    // Sends this game's fills when they are new or changed, unless it follows another game's for this quest.
    void SendAliases(uint32_t aQuestFormId) noexcept;
    // On the leader: offers the fills of every radiant quest running here, for a party whose saves each run their own.
    void SendRunningQuestAliases(const char* acpWhy) noexcept;
    void OnQuestAliases(const NotifyQuestAliases& acMessage) noexcept;
    // Applies another game's fills once, all together, when the quest runs here, every target is found and no scene plays.
    void ApplyWantedAliases(uint32_t aQuestFormId) noexcept;
    // Every couple of seconds: applies what waited, ends runs whose quest stopped, and resends this game's re-picks.
    void UpdateAliasSync() noexcept;
    void OnPartyInfoForAliases(const NotifyPartyInfo& acMessage) noexcept;
    void OnPartyJoinedForAliases(const PartyJoinedEvent&) noexcept;
    void ForgetAliasSync() noexcept;

    void OnConnected(const ConnectedEvent&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnPartyLeft(const PartyLeftEvent&) noexcept;

    BSTEventResult OnEvent(const TESQuestStartStopEvent*, const EventDispatcher<TESQuestStartStopEvent>*) override;
    BSTEventResult OnEvent(const TESQuestStageEvent*, const EventDispatcher<TESQuestStageEvent>*) override;
    BSTEventResult OnEvent(const TESQuestStageItemDoneEvent*, const EventDispatcher<TESQuestStageItemDoneEvent>*) override;
    BSTEventResult OnEvent(const TESSceneEvent*, const EventDispatcher<TESSceneEvent>*) override;
    BSTEventResult OnEvent(const TESSceneActionEvent*, const EventDispatcher<TESSceneActionEvent>*) override;
    BSTEventResult OnEvent(const TESScenePhaseEvent*, const EventDispatcher<TESScenePhaseEvent>*) override;
    BSTEventResult OnEvent(const TESTopicInfoEvent*, const EventDispatcher<TESTopicInfoEvent>*) override;
    BSTEventResult OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*) override;

    void OnQuestUpdate(const NotifyQuestUpdate&) noexcept;
    // On joining a party: what it was sent before. Only stages not reached here are taken, none is run again.
    void OnQuestCatchUp(const NotifyQuestCatchUp&) noexcept;
    void ApplyQuestUpdate(const NotifyQuestUpdate&, bool aCatchingUp) noexcept;
    // One caught-up step applied per frame, and none for a quest while its start is still under way. Steps that change
    // nothing here are passed over in the same frame, a bounded number of them (QuestCatchUpStep::TakesNextThisFrame).
    void RunCatchUp() noexcept;
    // One step of the catch-up; true when it was passed over: nothing applied here and nothing to wait for.
    bool RunCatchUpStep() noexcept;
    // Whether the quest the catch-up just started is ready for its next steps; gives up after 10 s.
    bool IsCatchUpStartDone(uint32_t aFormId, std::chrono::steady_clock::time_point aNow) noexcept;
    /**
     * Follow the party leader's civil war in this member's game, forward only.
     * The holds take the leader's owners and the journal's war objectives gain the leader's flags.
     * When the leader's war is behind this game's (an objective is completed or failed in this game but not in the leader's), its holds are left alone, as they would undo this game's own wins.
     * No objective loses a flag, and no hold is taken back while the leader's war is behind.
     *
     * @param acMessage The leader's war, as the server relays it.
     */
    void OnWarState(const NotifyWarState& acMessage) noexcept;
    // On the leader, in a party: sends the war when it changes, and every so often for whoever fell behind.
    void SendWarState() noexcept;
    /**
     * Forget what this member set from the leader's war, and which of the leader's states it found behind, so that the next state the leader sends is looked at afresh.
     */
    void ForgetAppliedWarState() noexcept;
    // On the leader, in a party: sends the quests its journal follows when they change, and every so often for whoever joined since.
    void SendTrackedQuests() noexcept;
    // On a member: its journal follows the leader's quests (QuestTracking).
    void OnTrackedQuests(const NotifyTrackedQuests&) noexcept;
    void ForgetTrackedQuests() noexcept;
    // A word of power a script taught this player goes to the party; one a party member was taught is taught here.
    void OnWordLearned(const WordLearnedEvent&) noexcept;
    void OnWordLearnedNotify(const NotifyWordLearned&) noexcept;
    void TeachWaitingWords() noexcept;
    void OnPartyJoined(const PartyJoinedEvent&) noexcept;
    // aSceneId and aPhase: the playing scene and phase that check the stage, 0 when it waits behind stages held before it.
    void HoldStage(TESQuest* apQuest, uint16_t aStage, uint32_t aSceneId, uint32_t aPhase) noexcept;
    // Applies every quest's held stages in order, without waiting for scenes: the server relays a stage only once, so dropping them would lose them.
    void FlushHeldStages(const char* acpReason) noexcept;
    // The quest's held stages, while it has stages to apply or fragments to wait for. Drops the stages that can't be
    // applied anymore.
    HeldStages* GetHeldStages(uint32_t aQuestFormId, TESQuest* apQuest) noexcept;

    World& m_world;
    // Keyed by quest form ID
    Map<uint32_t, HeldStages> m_heldStages{};

    WarState m_sentWarState{};
    // The quest steps the party was sent before this player joined, and the next one to take; Live for one that came
    // while those were still being taken, queued behind them so they keep their order.
    struct CatchUpStep
    {
        QuestStep Step;
        bool Live;
    };
    Vector<CatchUpStep> m_catchUp{};
    size_t m_catchUpNext{};
    // Quests whose caught-up run is left alone here: running or completed here already. Their stop is not taken either.
    Vector<uint32_t> m_catchUpSkipped{};
    // A quest the catch-up just started, whose next steps wait until its start is done.
    uint32_t m_catchUpStarting{};
    std::chrono::steady_clock::time_point m_catchUpStartedAt{};
    // The started run also stops later in the list: the party ended it, so no targets will come for it.
    bool m_catchUpRunEnded{};
    // Radiant quests the party started and finished while this player was away: none of their steps is taken here.
    Vector<uint32_t> m_catchUpLeftOut{};
    // The full queue is told once per catch-up.
    bool m_catchUpFullLogged{};
    // The leader asked for the party's steps after a load: until then, or until its catch-up is done, its war and tracked quests wait.
    std::chrono::steady_clock::time_point m_awaitCatchUpUntil{};
    // When the last catch-up was done; unset for never.
    std::chrono::steady_clock::time_point m_catchUpEndedAt{};
    // Quests the party's start was applied to, until they are seen running here: their stages are the new run's,
    // even while the game has not started it yet.
    Set<uint32_t> m_partyStarts{};
    // What this member last set or chose not to set from the leader's war, so that nothing is tried, and announced in the journal, again and again.
    WarState m_appliedWarState{};
    // The leader's last state found behind this game's war, so that it is logged once and not every time it comes again.
    WarState m_behindWarState{};
    std::chrono::steady_clock::time_point m_nextWarCheck{};
    std::chrono::steady_clock::time_point m_nextWarSend{};
    // The quests this leader's journal follows, as last sent.
    Vector<TrackedQuest> m_sentTracked{};
    std::chrono::steady_clock::time_point m_nextTrackedCheck{};
    std::chrono::steady_clock::time_point m_nextTrackedSend{};
    // The leader's quests this member's journal follows, and whether it follows a leader yet.
    std::vector<uint32_t> m_trackedFollowed{};
    bool m_followsLeader{false};
    // The leader's quests that did not run here, told once each.
    std::vector<uint32_t> m_trackedNotHereLogged{};

    QuestAliasSync::SendTracker m_aliasSent{};
    QuestAliasSync::Wanted m_aliasWanted{};
    std::chrono::steady_clock::time_point m_nextAliasPass{};
    size_t m_aliasPartySize{0};
    bool m_aliasWasLeader{false};

    entt::scoped_connection m_joinedConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_leftConnection;
    entt::scoped_connection m_questUpdateConnection;
    entt::scoped_connection m_questCatchUpConnection;
    entt::scoped_connection m_warStateConnection;
    entt::scoped_connection m_trackedQuestsConnection;
    entt::scoped_connection m_wordLearnedConnection;
    entt::scoped_connection m_wordLearnedNotifyConnection;
    entt::scoped_connection m_wordsPartyJoinedConnection;
    // Words a party member was taught while a save loaded here: the server tells each once, so they wait for the load to end.
    WordGrants m_wordGrants{};
    entt::scoped_connection m_questAliasesConnection;
    entt::scoped_connection m_aliasPartyInfoConnection;
    entt::scoped_connection m_aliasPartyJoinedConnection;
};
