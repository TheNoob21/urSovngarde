#pragma once

#include <Events/PacketEvent.h>
#include <ItemTakes.h>

#include <Structs/Inventory.h>

struct World;
struct Player;
struct PlayerLeaveEvent;
struct RequestItemTake;

/**
 * @brief Decides who gets an item two players took at once, and tells the other player's game to give it back.
 *
 * Out of a container or a body (InventoryService), a dropped item, and a placed item on a table or a shelf.
 * Each game hands the item to its own player at once, so the first take the server hears of stands with no wait.
 */
class ItemTakeService
{
public:
    ItemTakeService(World& aWorld, entt::dispatcher& aDispatcher);

    //! A take out of a container or a body: takes what the server gives out of aContent and returns how many that is.
    //! The rest went to another player first, and the taker's game is told. aMayRefuse false: given whatever the count.
    int32_t TakeOut(Player* apPlayer, uint32_t aServerId, Inventory& aContent, const Inventory::Entry& acItem, bool aMayRefuse) noexcept;

private:
    void OnItemTake(const PacketEvent<RequestItemTake>& acMessage) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;

    World& m_world;

    ItemTakes::Claims<ItemTakes::StackKey> m_stacks;
    ItemTakes::Claims<uint32_t> m_droppedItems;
    ItemTakes::Claims<uint64_t> m_placedItems;

    entt::scoped_connection m_itemTakeConnection;
    entt::scoped_connection m_playerLeaveConnection;
};
