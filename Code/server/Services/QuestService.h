#pragma once

#include <Events/PacketEvent.h>
#include <Structs/GameId.h>

#include <PartyWords.h>

struct World;
struct UpdateEvent;
struct RequestQuestUpdate;
struct RequestWarState;
struct RequestTrackedQuests;
struct RequestWordLearned;
struct RequestQuestCatchUp;

/**
 * @brief Dispatch quest sync messages.
 *
 * This service is currently not in use.
 */
class QuestService
{
public:
    QuestService(World& aWorld, entt::dispatcher& aDispatcher);

    // Gameplay:bEnableQuestCatchUp: a party keeps its quest steps for a player who joins later, and sends them.
    static bool IsCatchUpOn() noexcept;

    // The words of power each party was taught, for a member who joins later (PartyService::SendPartyJoinedEvent).
    const PartyWords& GetPartyWords() const noexcept { return m_partyWords; }
    void ForgetPartyWords(uint32_t aPartyId) { m_partyWords.Forget(aPartyId); }

    // Whether a member's game reached a quest step new to its party just now (SharedHomes::kSwitchAfterUseMs): its switches are taken then.
    [[nodiscard]] bool SteppedRecently(const Player* apPlayer) const noexcept;

private:
    void OnQuestChanges(const PacketEvent<RequestQuestUpdate>& aChanges) noexcept;
    void OnWarState(const PacketEvent<RequestWarState>& acMessage) noexcept;
    void OnTrackedQuests(const PacketEvent<RequestTrackedQuests>& acMessage) noexcept;
    void OnWordLearned(const PacketEvent<RequestWordLearned>& acMessage) noexcept;
    void OnQuestCatchUpAsked(const PacketEvent<RequestQuestCatchUp>& acMessage) noexcept;
    void NoteMemberStep(uint32_t aPlayerId) noexcept;

    World& m_world;

    entt::scoped_connection m_questUpdateConnection;
    entt::scoped_connection m_warStateConnection;
    entt::scoped_connection m_trackedQuestsConnection;
    entt::scoped_connection m_wordLearnedConnection;
    entt::scoped_connection m_questCatchUpConnection;

    // When each player's last ask again was answered (RequestQuestCatchUp), by player id.
    Map<uint32_t, std::chrono::steady_clock::time_point> m_catchUpAskedAt;
    // When each member last reached a quest step new to its party, by player id.
    Map<uint32_t, std::chrono::steady_clock::time_point> m_memberStepAt;

    PartyWords m_partyWords;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_joinConnection;
};
