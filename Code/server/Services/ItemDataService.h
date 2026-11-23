#pragma once

#include <Events/PacketEvent.h>

struct World;
struct RequestItemDataChange;

/**
 * @brief Keeps a player's items as they are when they change where they lie (ItemDataChange), and tells the players nearby.
 */
struct ItemDataService
{
    ItemDataService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~ItemDataService() noexcept = default;

    TP_NOCOPYMOVE(ItemDataService);

protected:
    void OnItemDataChange(const PacketEvent<RequestItemDataChange>& acMessage) noexcept;

private:
    World& m_world;

    entt::scoped_connection m_itemDataConnection;
};
