#pragma once

#include <Services/TakeUndo.h>

struct World;
struct TransportService;
struct DisconnectedEvent;
struct ItemTakeEvent;
struct NotifyItemTakeLost;

/**
 * @brief Gives back an item another player took first.
 *
 * Two players can take the same item at once: the last of a stack in a chest or on a body, a dropped item, a sword on a table.
 * Each game hands the item to its own player at once, with no wait. The server keeps the first take it hears of; the other
 * player's game takes the item back off its player, which the server and the other games then see as any removal.
 * A dropped or placed item's take is claimed here; a take out of a container or a body goes as its removal (InventoryService).
 */
struct ItemTakeService
{
    ItemTakeService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport);
    ~ItemTakeService() noexcept = default;

    TP_NOCOPYMOVE(ItemTakeService);

protected:
    void OnItemTake(const ItemTakeEvent& acEvent) noexcept;
    void OnNotifyItemTakeLost(const NotifyItemTakeLost& acMessage) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;

private:
    World& m_world;
    TransportService& m_transport;
    TakeUndo m_takes;

    entt::scoped_connection m_takeConnection;
    entt::scoped_connection m_lostConnection;
    entt::scoped_connection m_disconnectedConnection;
};
