#pragma once

#include <Structs/GameId.h>
#include <Structs/Inventory.h>
#include <Structs/ItemTakeSource.h>

//! The local player took an item another player may have taken at the same time; sent to the server already.
struct ItemTakeEvent
{
    ItemTakeSource From{};
    //! The container's or body's server id, or the dropped item's id.
    uint32_t Id{};
    //! A placed item's reference id, and its cell.
    GameId ObjectId{};
    GameId CellId{};
    //! What the player got, with its count.
    Inventory::Entry Item{};
    GameId WorldSpaceId{};
    glm::vec3 Position{};
};
