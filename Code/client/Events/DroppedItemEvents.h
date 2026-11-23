#pragma once

#include <Structs/Inventory.h>

//! The local player dropped an item; raised after the real drop, so the reference on the floor exists.
struct ItemDroppedEvent
{
    uint32_t RefFormId{};
    Inventory::Entry Item{};
};

//! The local player is picking up a reference that lies on the floor.
//! Raised before the pick-up runs, while the reference still exists to be named.
struct ItemPickedUpEvent
{
    uint32_t RefFormId{};
    //! Its handle (its incarnation): a hand item is known by it, not by its id alone.
    uint32_t RefHandle{};
    Inventory::Entry Item{};

    //! A placed reference the server keeps taken (PlacedTakes), named here because the reference is gone by the time the event runs.
    //! Activation sync takes it in the games that have it loaded now, but leaves books out, and HIGGS picks books up directly (SkipActivateBooks).
    bool Placed{};
    GameId ObjectId{};
    //! Scope is captured for every pickup, including a dropped reference whose handle changed on reload.
    GameId CellId{};
    GameId WorldSpaceId{};
    glm::vec3 Position{};
};
