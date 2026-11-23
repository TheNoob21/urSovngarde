#pragma once

#include <Structs/Inventory.h>

// An incoming count may move an item, but the other game does not dress this
// game's own follower. Keep all stack metadata and leave the received entry alone.
inline Inventory::Entry FollowerOwnerItem(const Inventory::Entry& acItem)
{
    Inventory::Entry item = acItem;
    item.ExtraWorn = false;
    item.ExtraWornLeft = false;
    return item;
}
