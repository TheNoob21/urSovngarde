#pragma once

#include <Events/EventDispatcher.h>
#include <Games/Events.h>

#include <atomic>

struct World;
struct ImguiService;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct NotifyPlayerList;
struct NotifyPartyInfo;
struct NotifyPartyInvite;
struct NotifyPartyJoined;
struct NotifyPartyLeft;
struct NotifyPartyPlugin;
struct NotifyPartyValue;
struct NotifyPartyFastTravel;

/**
 * @brief Manages the party of the local player.
 */
struct PartyService : BSTEventSink<TESLoadGameEvent>
{
    PartyService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransportService) noexcept;
    ~PartyService() = default;

    TP_NOCOPYMOVE(PartyService);

    [[nodiscard]] bool IsInParty() const noexcept { return m_inParty; }
    [[nodiscard]] bool IsLeader() const noexcept { return m_isLeader; }
    [[nodiscard]] uint32_t GetLeaderPlayerId() const noexcept { return m_leaderPlayerId; }

    const Vector<uint32_t>& GetPartyMembers() const noexcept { return m_partyMembers; }
    //! The party's players within 50 m of this one, this one included: what enemies are scaled to (GameplayFormulas::PartyEnemyHealth).
    [[nodiscard]] uint32_t GetPlayersNearby() const noexcept { return m_playersNearby; }
    const Map<uint32_t, String>& GetPlayers() const noexcept { return m_players; }
    //! The leader's fast travel may be followed (Gameplay:bEnablePartyFastTravel): the Sleep/Wait menu answers it, so it asks for no sleep then.
    [[nodiscard]] bool HasFollowOffer() const noexcept { return m_followOfferUntil != std::chrono::steady_clock::time_point{} || m_followAccepted; }
    //! The Sleep/Wait menu open now was opened to follow the leader's fast travel: its OK follows rather than waits (FollowMenuOk).
    [[nodiscard]] bool IsFollowAccepted() const noexcept { return m_followAccepted; }
    //! The follow menu's OK: the menu is closed from the next update, and the follow goes on as when the player closes it.
    void CloseFollowMenu() noexcept { m_closeFollowMenu.store(true); }
    //! Whether another player plays flat Skyrim rather than Skyrim VR, as the server's player list says.
    [[nodiscard]] bool IsFlatPlayer(uint32_t aPlayerId) const noexcept { return std::find(m_flatPlayers.begin(), m_flatPlayers.end(), aPlayerId) != m_flatPlayers.end(); }
    Map<uint32_t, uint64_t>& GetInvitations() noexcept { return m_invitations; }

    void CreateParty() const noexcept;
    void LeaveParty() const noexcept;
    void CreateInvite(const uint32_t aPlayerId) const noexcept;
    void AcceptInvite(const uint32_t aInviterId) const noexcept;
    void KickPartyMember(const uint32_t aPlayerId) const noexcept;
    void ChangePartyLeader(const uint32_t aPlayerId) const noexcept;

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnPlayerList(const NotifyPlayerList& acPlayerList) noexcept;
    void OnPartyInfo(const NotifyPartyInfo& acPartyInfo) noexcept;
    void OnPartyInvite(const NotifyPartyInvite& acPartyInvite) noexcept;
    void OnPartyJoined(const NotifyPartyJoined& acPartyJoined) noexcept;
    void OnPartyLeft(const NotifyPartyLeft& acPartyLeft) noexcept;
    void OnPartyPlugin(const NotifyPartyPlugin& acMessage) noexcept;
    void OnPartyValue(const NotifyPartyValue& acMessage) noexcept;
    void OnPartyFastTravel(const NotifyPartyFastTravel& acMessage) noexcept;
    BSTEventResult OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*) override;

private:
    void DestroyParty() noexcept;
    void UpdateWorldEncounters() noexcept;
    void CountPlayersNearby() noexcept;
    void SetPlayersNearby(uint32_t aPlayers) noexcept;
    // The leader tells the party of its fast travels; a member answers the offer with the Sleep/Wait menu.
    void RunFastTravelFollow() noexcept;
    void ForgetFollowOffer() noexcept;

    Map<uint32_t, String> m_players;
    Vector<uint32_t> m_flatPlayers;
    Map<uint32_t, uint64_t> m_invitations;
    uint64_t m_nextUpdate{0};

    bool m_inParty = false;
    bool m_isLeader = false;
    uint32_t m_leaderPlayerId = 0;
    Vector<uint32_t> m_partyMembers;
    uint32_t m_playersNearby = 1;

    std::chrono::steady_clock::time_point m_nextFollowCheck{};
    // Fast travels this game had queued at the last check (UI::FastTravelsQueued), and when to tell the party of a new one.
    std::optional<uint32_t> m_fastTravelsSeen{};
    std::chrono::steady_clock::time_point m_fastTravelSendAt{};
    // A member: until when the leader's last fast travel may be followed, and whether the Sleep/Wait menu took it.
    std::chrono::steady_clock::time_point m_followOfferUntil{};
    uint32_t m_followLeaderId = 0;
    bool m_followAccepted = false;
    // Set by the follow menu's OK (from the menu's callback), taken by RunFastTravelFollow.
    std::atomic<bool> m_closeFollowMenu{false};
    bool m_waitMenuWasOpen = false;

    World& m_world;
    TransportService& m_transport;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectConnection;
    entt::scoped_connection m_playerListConnection;
    entt::scoped_connection m_partyInfoConnection;
    entt::scoped_connection m_partyInviteConnection;
    entt::scoped_connection m_partyJoinedConnection;
    entt::scoped_connection m_partyLeftConnection;
    entt::scoped_connection m_partyPluginConnection;
    entt::scoped_connection m_partyValueConnection;
    entt::scoped_connection m_partyFastTravelConnection;
};
