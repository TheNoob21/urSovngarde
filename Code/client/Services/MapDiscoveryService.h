#pragma once

#include <Events/EventDispatcher.h>
#include <MapDiscoveries.h>

#include <atomic>

struct World;
struct TransportService;
struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct PartyJoinedEvent;
struct PartyLeftEvent;
struct NotifyMapDiscovery;
struct TESWorldSpace;

/**
 * @brief Map locations a party member discovers are discovered for the whole party (MapDiscoveries.h).
 * This game's markers are looked at every few seconds; what came on the map, or became a destination, goes to the server,
 * which tells the party and keeps it for a member who joins later. The party's discoveries are put on this map without the
 * game's discovery message, and again after a save loads.
 */
struct MapDiscoveryService final : BSTEventSink<TESLoadGameEvent>
{
    MapDiscoveryService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~MapDiscoveryService() noexcept = default;

    TP_NOCOPYMOVE(MapDiscoveryService);

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnConnected(const ConnectedEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnPartyJoined(const PartyJoinedEvent& acEvent) noexcept;
    void OnPartyLeft(const PartyLeftEvent& acEvent) noexcept;
    void OnNotifyMapDiscovery(const NotifyMapDiscovery& acMessage) noexcept;
    BSTEventResult OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*) override;

private:
    // A connection, a party joined or left: what came before belongs to another party.
    void NewTerm() noexcept;
    // The map markers of a world space and their flags, without the other players' markers (PlayerMarkerService).
    void CollectMarkers(TESWorldSpace* apWorldSpace) noexcept;
    void LookForDiscoveries() noexcept;
    // Puts the party's discoveries that this map does not have yet on it.
    void AddPartyDiscoveries() noexcept;

    World& m_world;
    TransportService& m_transport;

    MapDiscoveries::Watch m_watch;
    //! The party's discoveries in this term, by marker form id: whether one may travel there.
    Map<uint32_t, bool> m_party;
    //! The markers of the world space looked at last; persistent references, so the list lasts until a load.
    Vector<uint32_t> m_markers;
    uint32_t m_markersWorldSpace{};
    std::chrono::steady_clock::time_point m_nextLook{};
    bool m_addDue{};
    std::atomic<bool> m_loaded{};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_partyJoinedConnection;
    entt::scoped_connection m_partyLeftConnection;
    entt::scoped_connection m_notifyConnection;
};
