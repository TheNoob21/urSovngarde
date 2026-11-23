#pragma once

#include <Structs/ItemDataChange.h>

struct TESForm;
struct TESBoundObject;
struct TESObjectREFR;
struct AlchemyItem;
struct ExtraDataList;

//! What an item carries beyond its base form and the extra data Inventory::Entry always had: a player's name for it,
//! and a made potion or poison, which exists only in the game that made it and travels as what it does (ItemData).
namespace ItemExtraData
{
//! Sets the entry's base form. A made potion or poison gets the id every game sends it by, and its effects.
//! False when the form cannot be told to the other games (a plugin they lack, a run-time form of another kind): BaseId is then empty.
bool SetBase(Inventory::Entry& arEntry, const TESForm* apBase) noexcept;

//! Reads the player's name for the item, a made poison on it and the id of a made enchantment into the entry.
//! Called by TESObjectREFR::GetItemFromExtraData once it has read the rest.
void Read(Inventory::Entry& arEntry, ExtraDataList* apExtraDataList) noexcept;

//! The base form here: a plugin's form, or the made potion or poison with the entry's effects, made here if this game has none.
TESBoundObject* GetBase(const Inventory::Entry& acEntry) noexcept;

//! The poison on the entry's weapon here, made here when it is a made one.
AlchemyItem* GetPoison(const Inventory::Entry& acEntry) noexcept;

//! Gives the item the player's name for it, as renaming at an enchanter does.
void SetName(ExtraDataList* apExtraDataList, const TiltedPhoques::String& acName) noexcept;

//! Changes the extra data of the stack apExtraDataList where it lies, to what acNew has.
void Update(ExtraDataList* apExtraDataList, const Inventory::Entry& acNew) noexcept;

//! The stack of apItem in apOwner's inventory with acEntry's extra data, worn or not; none for an entry without extra data.
ExtraDataList* FindStack(TESObjectREFR* apOwner, const TESForm* apItem, const Inventory::Entry& acEntry) noexcept;

//! Changes the items of apOwner that were acChange.Old to acChange.New, where they lie. Returns how many changed.
int32_t Apply(TESObjectREFR* apOwner, const ItemDataChange& acChange) noexcept;
} // namespace ItemExtraData
