#pragma once

#include <Structs/Inventory.h>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct NotifyItemDataChange;
struct NotifyInventoryChanges;
struct InventoryChangeEvent;

/**
 * @brief Items that change where they lie, which no add or remove hook sees: a weapon or armour tempered, a weapon's
 * charge used or its poison put on and used up, a soul caught in a gem, a name given.
 *
 * This player's weapons, armour and soul gems are read once a second and compared (ItemDataChanges::Find). What changed
 * goes to the server, which keeps it with the player's inventory, and to the other players, whose copy of this player
 * changes the same way (ItemExtraData::Apply).
 */
struct ItemDataService
{
    ItemDataService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher);
    ~ItemDataService() noexcept = default;

    TP_NOCOPYMOVE(ItemDataService);

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnNotifyItemDataChange(const NotifyItemDataChange& acMessage) noexcept;
    void OnInventoryChange(const InventoryChangeEvent& acEvent) noexcept;
    void OnNotifyInventoryChanges(const NotifyInventoryChanges& acMessage) noexcept;

private:
    World& m_world;
    TransportService& m_transport;

    //! The player's items as last read, for the server id they were read under.
    Inventory m_last{};
    uint32_t m_lastServerId{};
    bool m_hasLast{};
    std::chrono::steady_clock::time_point m_nextRead{};
    //! What the player's add and remove hooks, and other players' changes to this player, said since the last reading.
    Vector<Inventory::Entry> m_deltas{};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_notifyConnection;
    entt::scoped_connection m_inventoryConnection;
    entt::scoped_connection m_inventoryNotifyConnection;
};
