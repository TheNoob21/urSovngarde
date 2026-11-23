#pragma once

#include <Structs/Inventory.h>

struct World;
struct Actor;
struct TransportService;

/**
 * Reads again what a local NPC wears after its equipment changes are sent, and corrects the other games' copies when it differs.
 * Its owner's game sends each equip and unequip its hooks see, but the game also equips through paths they do not see.
 * After an unequip the hooks saw, a quest put a shield back on with none: the other game kept the unequip.
 * The correction is an equipment snapshot, and a snapshot corrects the copy's hands, spells, worn armor and ammo (ApplyWornItems).
 */
namespace NpcEquipmentCheck
{
/**
 * Check whether two equipment lists hold the same items, worn in the same hands, with the same spells.
 * Charges and other extra data are not compared.
 *
 * @param acLhs One equipment list.
 * @param acRhs The other equipment list.
 * @return True when both list the same items in the same order, worn the same way, with the same magic equipment.
 */
bool HasSameWornItems(const Inventory& acLhs, const Inventory& acRhs) noexcept;

/**
 * Check whether an equipment list has an item worn, in either hand or as armor.
 *
 * @param acInventory The equipment list.
 * @param acItemId The item, as the server names it.
 * @return True when the list has that item worn.
 */
bool IsWorn(const Inventory& acInventory, const GameId& acItemId) noexcept;

/**
 * Queue two checks of what a local NPC wears after one of its equipment changes is sent: 1 s and 3 s later.
 * A difference from what was sent goes out as an equipment snapshot.
 * A check already waiting for that NPC is merged into the new one, so a change the game undid at once is not lost (NpcEquipmentQueue::Queue).
 *
 * @param aFormId The NPC's form id in this game.
 * @param aServerId The NPC's id on the server.
 * @param aOwnershipEpoch The ownership epoch the change was sent with.
 * @param acSent The equipment sent with the change.
 * @param aUndone True when the change was already undone when it was sent, so the first check sends what the NPC wears whatever it finds.
 */
void Note(uint32_t aFormId, uint32_t aServerId, uint32_t aOwnershipEpoch, const Inventory& acSent, bool aUndone) noexcept;

/**
 * Run the checks that are due, and send an equipment snapshot for each NPC that wears other items than were sent or had a change undone.
 * A check is dropped when its NPC is no longer this game's at the same epoch, and all are dropped while not connected.
 * Called every frame.
 *
 * @param aWorld The world, for the NPCs this game owns.
 * @param aTransport The connection the snapshots are sent through.
 */
void Run(World& aWorld, TransportService& aTransport) noexcept;

/**
 * Remember what a remote copy's owner wears, from the copy's spawn or assignment data, so its next snapshot can take off what the owner took off.
 *
 * @param apActor The copy. Nothing is kept for an actor this game owns.
 * @param acOwnerInventory The inventory its owner's game sent, with the worn flags.
 * @param aAddOnly True for a single equip or unequip message: its pieces are added to what is remembered, and nothing is forgotten.
 *        Only a snapshot, which is compared with what the copy wears, or the spawn data replaces the list; otherwise a piece the owner
 *        lost unseen before an unrelated message would be forgotten while the copy still wears it, and never come off.
 */
void NoteOwnerEquipment(Actor* apActor, const Inventory& acOwnerInventory, bool aAddOnly = false) noexcept;

/**
 * Make an NPC's copy wear every armor piece its owner wears (shield, rings, amulet, helmet and the rest) and its ammo, from an equipment snapshot.
 * A piece of the copy comes off when a piece its owner wears takes its slot, or when its owner wore it before (spawn data or an earlier snapshot) and no longer does.
 * A piece its owner was never known to wear stays when nothing takes its slot: its owner may wear one there that this game cannot name.
 * A piece the copy lacks is added to its inventory first; when that fails, the copy keeps its own.
 *
 * @param apActor The copy.
 * @param acOwnerEquipment The equipment its owner's game sent.
 */
void ApplyWornItems(Actor* apActor, const Inventory& acOwnerEquipment) noexcept;
} // namespace NpcEquipmentCheck
