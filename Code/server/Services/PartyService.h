#pragma once

#include <Events/PacketEvent.h>
#include <QuestAliasHistory.h>
#include <QuestHistory.h>
#include <MapDiscoveries.h>
#include <PartyWeather.h>
#include <ReferenceSwitches.h>
#include <Messages/NotifySetWaypoint.h>

struct World;
struct UpdateEvent;
struct PlayerJoinEvent;
struct PlayerLeaveEvent;
struct PartyInviteRequest;
struct PartyAcceptInviteRequest;
struct PartyLeaveRequest;
struct RequestPartyFastTravel;
struct RequestPartyFollow;
struct NotifyPartyInfo;
struct PartyCreateRequest;
struct PartyChangeLeaderRequest;
struct PartyKickRequest;

/**
 * @brief Manages every party in the server.
 */
struct RequestPartyPlugin;
struct RequestPartyValue;

struct PartyService
{
    struct Party
    {
        uint32_t LeaderPlayerId;
        Vector<Player*> Members;
        PartyWeather Weathers{};
        // Stages each quest reached since it started, as relayed to the party, with when each was first relayed
        Map<GameId, Map<uint16_t, std::chrono::steady_clock::time_point>> QuestStages{};
        // References the leader's scripts switched on (true) or off, the latest state of each (ObjectService::OnReferenceEnableRequest)
        ReferenceSwitches ReferenceEnabled{};
        // Numbers the leader's plugins set for the party, the latest of each key (RequestPartyValue)
        Map<String, float> PluginValues{};
        // Quest starts, stages and stops relayed to the party, for a player who joins it later
        QuestHistory Quests{};
        QuestAliasHistory QuestAliasFills{};
        // Members who may follow the leader's last fast travel, and since when (Gameplay:bEnablePartyFastTravel)
        Map<uint32_t, std::chrono::steady_clock::time_point> FollowOffers{};
        // Map markers its members discovered while in it (MapService::OnMapDiscoveryRequest)
        MapDiscoveries::Party Discoveries{};
        std::optional<NotifySetWaypoint> Waypoint{};
    };

    PartyService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~PartyService() noexcept = default;

    TP_NOCOPYMOVE(PartyService);

    const Party* GetById(uint32_t aId) const noexcept;
    bool IsPlayerInParty(Player* const apPlayer) const noexcept;
    bool IsPlayerLeader(const Player* const apPlayer) const noexcept;
    Party* GetPlayerParty(Player* const apPlayer) noexcept;
    // Every player's list of the others, with their names; apPlayer, if given, gets none.
    void BroadcastPlayerList(Player* apPlayer = nullptr) const noexcept;

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;
    void OnPartyInvite(const PacketEvent<PartyInviteRequest>& acPacket) noexcept;
    void OnPartyAcceptInvite(const PacketEvent<PartyAcceptInviteRequest>& acPacket) noexcept;
    void OnPartyLeave(const PacketEvent<PartyLeaveRequest>& acPacket) noexcept;
    void OnPartyCreate(const PacketEvent<PartyCreateRequest>& acPacket) noexcept;
    void OnPartyChangeLeader(const PacketEvent<PartyChangeLeaderRequest>& acPacket) noexcept;
    void OnPartyKick(const PacketEvent<PartyKickRequest>& acPacket) noexcept;
    void OnPartyPlugin(const PacketEvent<RequestPartyPlugin>& acPacket) noexcept;
    void OnPartyValue(const PacketEvent<RequestPartyValue>& acPacket) noexcept;
    void OnPartyFastTravel(const PacketEvent<RequestPartyFastTravel>& acPacket) noexcept;
    void OnPartyFollow(const PacketEvent<RequestPartyFollow>& acPacket) noexcept;
    void RemovePlayerFromParty(Player* apPlayer) noexcept;

    void BroadcastPartyInfo(uint32_t aPartyId) const noexcept;

private:
    World& m_world;

    TiltedPhoques::Map<uint32_t, Party> m_parties;
    uint32_t m_nextId{0};
    uint64_t m_nextInvitationExpire{0};

    entt::scoped_connection m_updateEvent;
    entt::scoped_connection m_playerJoinConnection;
    entt::scoped_connection m_playerLeaveConnection;
    entt::scoped_connection m_partyInviteConnection;
    entt::scoped_connection m_partyAcceptInviteConnection;
    entt::scoped_connection m_partyLeaveConnection;
    entt::scoped_connection m_partyCreateConnection;
    entt::scoped_connection m_partyChangeLeaderConnection;
    entt::scoped_connection m_partyKickConnection;
    entt::scoped_connection m_partyPluginConnection;
    entt::scoped_connection m_partyValueConnection;
    entt::scoped_connection m_partyFastTravelConnection;
    entt::scoped_connection m_partyFollowConnection;

    void SendPartyJoinedEvent(Party& aParty, Player* aPlayer) noexcept;
    void CreateParty(Player* apLeader) noexcept;
};
