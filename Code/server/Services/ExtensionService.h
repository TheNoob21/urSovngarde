#pragma once

#include <Events/PacketEvent.h>
#include <Messages/NotifyExtension.h>
#include <Services/ExtensionRecords.h>
#include <map>
#include <deque>
#include <set>
#include <tuple>

struct World;
struct Player;
struct RequestExtension;
struct UpdateEvent;
struct PlayerLeaveEvent;

struct ExtensionService
{
    ExtensionService(World& aWorld, entt::dispatcher& aDispatcher);
    TP_NOCOPYMOVE(ExtensionService);
    bool Reserved(uint32_t aActor, uint32_t aExceptAddon = 0) const;

private:
    using Clock = std::chrono::steady_clock;
    struct Rate
    {
        Clock::time_point Start{};
        uint32_t Messages{};
        size_t Bytes{};
        bool Take(Clock::time_point aNow, size_t aSize, uint32_t aMax, size_t aMaxBytes);
    };
    struct Peer
    {
        Rate Total;
        Rate Records;
        std::set<uint32_t> Subscriptions;
        std::map<uint32_t, Clock::time_point> Refresh;
        std::map<uint32_t, Clock::time_point> NextRefresh;
        uint32_t Replays{};
        std::map<uint32_t, Rate> Addons;
        CellIdComponent Cell;
        std::map<std::pair<uint32_t, uint64_t>, uint64_t> Seen;
        std::deque<std::pair<uint32_t, uint64_t>> Pending;
    };
    struct Retained
    {
        NotifyExtension Message;
        std::optional<uint32_t> Party;
        std::set<uint32_t> Delivered;
        Clock::time_point Until{};
    };
    struct AreaState
    {
        NotifyExtension Message;
        CellIdComponent Cell;
        uint32_t Owner{};
        std::set<uint32_t> Delivered;
    };
    struct Lease
    {
        uint32_t Player;
        uint32_t Subject;
        uint32_t Epoch;
        Clock::time_point Until;
    };
    using StateKey = std::tuple<uint32_t, uint32_t, uint64_t>;
    using LeaseKey = std::tuple<uint32_t, uint32_t, uint32_t, uint64_t>;

    void OnRequest(const PacketEvent<RequestExtension>& acEvent);
    void OnLeave(const PlayerLeaveEvent& acEvent);
    void OnUpdate(const UpdateEvent&);
    bool Authorize(Player& aPlayer, ExtensionData& aMessage) const;
    bool Visible(const Player& acViewer, const ExtensionData& acMessage, std::optional<uint32_t> aParty) const;
    bool WithinBudget(Player& aSender, const ExtensionData& acMessage, Clock::time_point aNow);
    bool TakeLease(uint32_t aPlayer, const ExtensionData& acMessage, Clock::time_point aNow);
    void Relay(Retained& aState, bool aNew);
    void Retire(const Retained& acState, Player* apViewer = nullptr) const;
    void OnReservation(Player& aSender, Retained& aState, Clock::time_point aNow);
    void EndReservation(Retained& aState);
    ExtensionRecords::Area AreaOf(const CellIdComponent& acCell) const;
    void OnAreaState(Player& aSender, const ExtensionData& acMessage);
    void RelayArea(AreaState& aState, bool aNew);
    void OnRecord(Player& aSender, const ExtensionData& acMessage);
    void ReplayRecords(Player& aViewer, Peer& aPeer) const;

    World& m_world;
    ExtensionRecords::Store m_store;
    std::map<uint32_t, Peer> m_peers;
    std::map<std::pair<uint32_t, uint64_t>, uint32_t> m_writers;
    std::map<StateKey, Retained> m_states;
    std::map<uint32_t, Retained> m_reservations;
    std::map<LeaseKey, Lease> m_leases;
    std::map<std::pair<uint32_t, uint64_t>, AreaState> m_areas;
    Clock::time_point m_nextReplay{};
    entt::scoped_connection m_request;
    entt::scoped_connection m_update;
    entt::scoped_connection m_leave;
};
