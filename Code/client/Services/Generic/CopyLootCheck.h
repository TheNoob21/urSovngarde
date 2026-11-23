#pragma once

#include <Structs/Inventory.h>

struct Actor;

// A copy's items, the same kinds as its owner's: what a player finds on its body is what its owner's game has.
// Its whole inventory is set from its owner's, but the game can still add the copy's own gear afterwards (seen in a battle: a copy holding its own gear as well as its owner's).
namespace CopyLootCheck
{
// After a copy's whole inventory is set from its owner's: its items are read again 2 s, 6 s and 10 s later.
// A living copy holding other kinds than its owner is logged and set again, at most twice; a difference left at the last read is logged as kept.
void Note(Actor* apActor, const Inventory& acOwnerInventory) noexcept;

// An inventory change from the owner, applied to the copy: applied to what the copy should hold too.
void OnOwnerChange(uint32_t aFormId, const Inventory::Entry& acItem, bool aDrop) noexcept;

// An equipment change or snapshot from the owner: what the copy should wear and hold in its hands from now on, so that a later
// set does not put back what it wore when it appeared.
void OnOwnerEquipment(uint32_t aFormId, const Inventory& acOwnerEquipment) noexcept;

// Each frame: the checks that are due.
void Run() noexcept;
} // namespace CopyLootCheck
