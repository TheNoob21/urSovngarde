#pragma once

#include <cstdint>

//! Where an item a player took came from, as it travels in messages.
enum class ItemTakeSource : uint8_t
{
    //! Out of a container or a body, named by its server id.
    kInventory = 0,
    //! A dropped item the server keeps, named by its id (DroppedItemService).
    kDroppedItem = 1,
    //! A placed item, a sword on a table, named by its reference id.
    kPlacedItem = 2,
};
