#pragma once

#include <Events/PacketEvent.h>

#include <Structs/GameId.h>
#include <Structs/GridCellCoords.h>
#include <Structs/Inventory.h>
#include <Structs/Vector3_NetQuantize.h>

#include <ObjectMover.h>
#include <PropBodyRelay.h>
#include <DroppedItemRecord.h>
#include <DroppedItemTrust.h>
#include <DeferredSave.h>
#include <ViewerResends.h>

struct World;
struct Player;
struct CellIdComponent;
struct RequestDroppedItemAdd;
struct RequestDroppedItemRemove;
struct RequestDroppedItemMove;
struct RequestWorldObjectMove;
struct RequestPropBodies;
struct RequestItemTake;
struct PlayerLeaveEvent;
struct UpdateEvent;
struct EnterInteriorCellRequest;
struct EnterExteriorCellRequest;
struct ShiftGridCellRequest;

/**
 * @brief Remembers items players drop, and gives them to everyone who comes near.
 *
 * Each item is sent to everyone in range when dropped and to anyone entering its cell, until somebody picks it up.
 * The list is saved to disk, so items outlive a server restart.
 */
class DroppedItemService
{
public:
    DroppedItemService(World& aWorld, entt::dispatcher& aDispatcher);
    ~DroppedItemService();
    void RecordPlacedTake(const RequestItemTake& acMessage, Player* apPlayer) noexcept;

    using Item = DroppedItemRecord;

    //! The saved list, for tests and diagnostics.
    [[nodiscard]] const std::map<uint32_t, Item>& Items() const noexcept { return m_items; }

private:
    void OnAdd(const PacketEvent<RequestDroppedItemAdd>& acMessage) noexcept;
    void OnRemove(const PacketEvent<RequestDroppedItemRemove>& acMessage) noexcept;
    void OnMove(const PacketEvent<RequestDroppedItemMove>& acMessage) noexcept;
    void OnObjectMove(const PacketEvent<RequestWorldObjectMove>& acMessage) noexcept;
    void RecordObjectMove(const RequestWorldObjectMove& acMessage, Player* apPlayer) noexcept;
    void OnPropBodies(const PacketEvent<RequestPropBodies>& acMessage) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnEnterInteriorCell(const PacketEvent<EnterInteriorCellRequest>& acMessage) const noexcept;
    void OnEnterExteriorCell(const PacketEvent<EnterExteriorCellRequest>& acMessage) const noexcept;
    void OnShiftGridCell(const PacketEvent<ShiftGridCellRequest>& acMessage) const noexcept;

    //! Everything within range of where a player now stands, to that player.
    void SendInRange(Player* apPlayer, const CellIdComponent& acViewer) const noexcept;
    //! aRequestId: the add's request id, only in the reply to the player who added it.
    void SendItem(Player* apPlayer, const Item& acItem, uint32_t aRequestId = 0, bool aReplay = false) const noexcept;

    void Load() noexcept;
    void Save() const noexcept;

    //! Where a placed world object was last seen, or that a player took it, for anyone who comes near later.
    //! The resting places are saved with the dropped items; the takes last for the session.
    struct ObjectPlace
    {
        GameId ObjectId{};
        GameId CellId{};
        GameId WorldSpaceId{};
        Vector3_NetQuantize Position{};
        glm::vec3 Rotation{};
        bool Taken{};
        //! When it was last moved, while it has not come to rest since; empty once it rests.
        std::chrono::steady_clock::time_point MovedAt{};
        //! When any player last moved or took it; the oldest place goes first when the server keeps too many.
        std::chrono::steady_clock::time_point LastSeen{};
    };

    //! What each player was last sent of the items and places in its range, and that range (ViewerResends).
    struct ViewerSends
    {
        GameId Cell{};
        GameId WorldSpaceId{};
        GridCellCoords CenterCoords{};
        //! Where its last grid shift was centred; a cell entry moves CenterCoords, not this.
        GameId ShiftWorldSpaceId{};
        GridCellCoords ShiftCenter{};
        std::map<uint32_t, ViewerResends::Record> Items;
        std::map<uint64_t, ViewerResends::Record> Objects;
    };

    //! Places kept at most, as many as the file keeps.
    static constexpr size_t kMaxObjects = 4096;
    //! The mover maps are swept of idle movers once they hold more than this.
    static constexpr size_t kMoversKept = 256;

    World& m_world;
    std::map<uint32_t, Item> m_items;
    std::map<uint64_t, ObjectPlace> m_objects;
    //! Whose moves are taken for each dropped item and placed object, while one of them is moving it.
    std::map<uint32_t, ObjectMover> m_itemMovers;
    std::map<uint64_t, ObjectMover> m_objectMovers;
    //! Whose swing of a placed object's hinged bodies is passed on, while one player's is.
    std::map<uint64_t, PropBodyRelay> m_bodyMovers;
    std::map<uint32_t, PropBodyRelay::Rate> m_bodyRates;
    //! Moves of dropped items and placed objects taken from each player per second, as for hinged bodies.
    std::map<uint32_t, PropBodyRelay::Rate> m_moveRates;
    //! Items each player may still add: a burst for the announcements after a load, then a few a second.
    std::map<uint32_t, DroppedItemTrust::AddBudget> m_addBudgets;
    uint32_t m_nextId{1};
    //! By player id.
    mutable std::map<uint32_t, ViewerSends> m_sent;
    //! Changes are written from the update, a few seconds apart at most.
    DeferredSave m_save;

    entt::scoped_connection m_addConnection;
    entt::scoped_connection m_removeConnection;
    entt::scoped_connection m_moveConnection;
    entt::scoped_connection m_objectMoveConnection;
    entt::scoped_connection m_propBodiesConnection;
    entt::scoped_connection m_leaveConnection;
    entt::scoped_connection m_interiorConnection;
    entt::scoped_connection m_exteriorConnection;
    entt::scoped_connection m_gridConnection;
    entt::scoped_connection m_updateConnection;
};
