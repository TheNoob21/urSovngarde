#pragma once

#include <algorithm>

#include <Structs/Inventory.h>

//! Items of one stack whose extra data changed where they lie: tempered, charge used, poison put on or used up, a soul caught.
//! No item comes or goes, so no add or remove hook sees it.
struct ItemDataChange
{
    bool operator==(const ItemDataChange& acRhs) const noexcept { return Old == acRhs.Old && New == acRhs.New; }
    bool operator!=(const ItemDataChange& acRhs) const noexcept { return !(*this == acRhs); }

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    //! The items as they were; Count is how many changed. Never worn: what is worn is equipment, sent on its own.
    Inventory::Entry Old{};
    //! The same items now, with the same Count.
    Inventory::Entry New{};
};

namespace ItemDataChanges
{
//! A message with more changes than this keeps only the first ones.
static constexpr size_t kMaxChanges = 32;
static constexpr uint32_t kRunTimeMod = 0xFFFFFFFF;

//! An item another game can make: a plugin's form, or a run-time one sent as what it does.
inline bool IsSyncable(const Inventory::Entry& acEntry) noexcept
{
    if (acEntry.BaseId.ModId == kRunTimeMod && !acEntry.Data.Base.IsSet())
        return false;
    if (acEntry.ExtraPoisonId.ModId == kRunTimeMod && !acEntry.Data.Poison.IsSet())
        return false;
    return true;
}

//! The entry as a stack of items: its worn flags are equipment, not item data.
inline Inventory::Entry Stack(Inventory::Entry aEntry) noexcept
{
    aEntry.ExtraWorn = false;
    aEntry.ExtraWornLeft = false;
    return aEntry;
}

//! One entry for each different stack, worn flags taken off, positive counts only.
inline Vector<Inventory::Entry> Stacks(const Inventory& acInventory) noexcept
{
    Vector<Inventory::Entry> stacks;
    for (const Inventory::Entry& acEntry : acInventory.Entries)
    {
        if (acEntry.Count <= 0)
            continue;
        const Inventory::Entry cStack = Stack(acEntry);
        const auto it = std::find_if(stacks.begin(), stacks.end(), [&cStack](const Inventory::Entry& acOther) { return acOther.CanBeMerged(cStack); });
        if (it != stacks.end())
            it->Count += cStack.Count;
        else
            stacks.push_back(cStack);
    }
    return stacks;
}

//! The inventory as its add and remove hooks say it became since acBefore: a reading that differs from it in place only
//! changed in place. An item that came or went in the same second is then not taken for a change.
inline Inventory Expect(const Inventory& acBefore, const Vector<Inventory::Entry>& acDeltas) noexcept
{
    Inventory expected{};
    expected.Entries = Stacks(acBefore);
    for (const Inventory::Entry& acDelta : acDeltas)
        expected.AddOrRemoveEntry(Stack(acDelta));
    return expected;
}

//! How alike two stacks of one base are, so a change pairs a stack with what it became and not with a neighbour.
inline int Likeness(const Inventory::Entry& acLhs, const Inventory::Entry& acRhs) noexcept
{
    return (acLhs.ExtraEnchantId == acRhs.ExtraEnchantId ? 4 : 0) + (acLhs.Data.DisplayName == acRhs.Data.DisplayName ? 2 : 0) + (acLhs.ExtraHealth == acRhs.ExtraHealth ? 1 : 0) +
           (acLhs.ExtraPoisonId == acRhs.ExtraPoisonId ? 1 : 0) + (acLhs.ExtraSoulLevel == acRhs.ExtraSoulLevel ? 1 : 0);
}

//! The changes in place between two readings of one inventory.
//! A base whose count changed is left out: its add or remove hook sends it, and the next reading starts from there.
inline Vector<ItemDataChange> Find(const Inventory& acBefore, const Inventory& acAfter) noexcept
{
    const Vector<Inventory::Entry> cBefore = Stacks(acBefore);
    const Vector<Inventory::Entry> cAfter = Stacks(acAfter);

    Vector<ItemDataChange> changes;
    Vector<GameId> bases;
    for (const Inventory::Entry& acEntry : cBefore)
    {
        if (std::find(bases.begin(), bases.end(), acEntry.BaseId) == bases.end())
            bases.push_back(acEntry.BaseId);
    }

    for (const GameId& acBase : bases)
    {
        // What left and what came of this base, stack by stack.
        Vector<Inventory::Entry> gone;
        Vector<Inventory::Entry> came;
        int64_t total = 0;
        for (const Inventory::Entry& acEntry : cBefore)
        {
            if (acEntry.BaseId != acBase)
                continue;
            total += acEntry.Count;
            const auto it = std::find_if(cAfter.begin(), cAfter.end(), [&acEntry](const Inventory::Entry& acOther) { return acOther.CanBeMerged(acEntry); });
            const int32_t cLeft = acEntry.Count - (it != cAfter.end() ? it->Count : 0);
            if (cLeft > 0)
            {
                gone.push_back(acEntry);
                gone.back().Count = cLeft;
            }
        }
        for (const Inventory::Entry& acEntry : cAfter)
        {
            if (acEntry.BaseId != acBase)
                continue;
            total -= acEntry.Count;
            const auto it = std::find_if(cBefore.begin(), cBefore.end(), [&acEntry](const Inventory::Entry& acOther) { return acOther.CanBeMerged(acEntry); });
            const int32_t cNew = acEntry.Count - (it != cBefore.end() ? it->Count : 0);
            if (cNew > 0)
            {
                came.push_back(acEntry);
                came.back().Count = cNew;
            }
        }
        if (total != 0)
            continue;

        for (Inventory::Entry& old : gone)
        {
            while (old.Count > 0)
            {
                auto best = came.end();
                int bestLikeness = -1;
                for (auto it = came.begin(); it != came.end(); ++it)
                {
                    if (it->Count <= 0)
                        continue;
                    const int cLikeness = Likeness(old, *it);
                    if (cLikeness > bestLikeness)
                    {
                        bestLikeness = cLikeness;
                        best = it;
                    }
                }
                if (best == came.end())
                    break;
                const int32_t cMoved = std::min(old.Count, best->Count);
                ItemDataChange change{old, *best};
                change.Old.Count = cMoved;
                change.New.Count = cMoved;
                changes.push_back(std::move(change));
                old.Count -= cMoved;
                best->Count -= cMoved;
            }
        }
    }
    return changes;
}

//! Applies a change to a kept inventory (the server's copy). Returns how many items took the new data.
//! The items keep where they were worn. Old items the copy lacks are taken from another stack of the base only while
//! the copy has no item with the new data: a game whose hooks did see the change already sent it as a remove and an add.
//! Never makes a count negative.
inline int32_t Apply(Inventory& aInventory, const ItemDataChange& acChange) noexcept
{
    if (acChange.Old.Count <= 0 || acChange.Old.BaseId != acChange.New.BaseId)
        return 0;

    const Inventory::Entry cOld = Stack(acChange.Old);
    const Inventory::Entry cNew = Stack(acChange.New);
    const auto cIsOld = [&cOld](const Inventory::Entry& acEntry)
    {
        return acEntry.Count > 0 && Stack(acEntry).CanBeMerged(cOld);
    };
    const auto cIsNew = [&cNew](const Inventory::Entry& acEntry)
    {
        return acEntry.Count > 0 && Stack(acEntry).CanBeMerged(cNew);
    };

    struct Moved
    {
        bool Worn;
        bool WornLeft;
        int32_t Count;
    };
    Vector<Moved> moved;
    int32_t left = acChange.Old.Count;
    const auto cTake = [&](const auto& acWhich)
    {
        for (Inventory::Entry& entry : aInventory.Entries)
        {
            if (left <= 0)
                break;
            if (!acWhich(entry))
                continue;
            const int32_t cCount = std::min(left, entry.Count);
            entry.Count -= cCount;
            left -= cCount;
            moved.push_back({entry.ExtraWorn, entry.ExtraWornLeft, cCount});
        }
    };
    cTake(cIsOld);
    // Only a stack that is the same item but for what changes in place (temper, charge, poison, soul): the enchantment and name agree.
    if (left == acChange.Old.Count && std::none_of(aInventory.Entries.begin(), aInventory.Entries.end(), cIsNew))
        cTake(
            [&cOld, &cNew](const Inventory::Entry& acEntry)
            {
                return acEntry.Count > 0 && acEntry.BaseId == cOld.BaseId && acEntry.ExtraEnchantId == cOld.ExtraEnchantId && acEntry.Data.DisplayName == cOld.Data.DisplayName &&
                       !Stack(acEntry).CanBeMerged(cNew);
            });

    aInventory.RemoveByFilter([](const Inventory::Entry& acEntry) { return acEntry.Count <= 0; });

    int32_t total = 0;
    for (const Moved& acMoved : moved)
    {
        Inventory::Entry entry = cNew;
        entry.ExtraWorn = acMoved.Worn;
        entry.ExtraWornLeft = acMoved.WornLeft;
        entry.Count = acMoved.Count;
        aInventory.AddOrRemoveEntry(entry);
        total += acMoved.Count;
    }
    return total;
}
} // namespace ItemDataChanges
