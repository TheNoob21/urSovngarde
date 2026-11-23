#pragma once

#include <Structs/Inventory.h>

#include <algorithm>
#include <iterator>
#include <tuple>

//! Which kinds of item a copy holds that its owner does not, and the other way round.
//! A kind is a base form held with a count above 0, whatever its extra data or where it is worn.
//! Plain values in and out, so the unit tests check the rule without a game (CopyLootCheck applies it).
namespace LootKinds
{
struct Difference
{
    Vector<GameId> Extra{};
    Vector<GameId> Missing{};

    [[nodiscard]] bool IsEmpty() const noexcept { return Extra.empty() && Missing.empty(); }
};

namespace Detail
{
inline bool Before(const GameId& acLhs, const GameId& acRhs) noexcept
{
    return std::tie(acLhs.ModId, acLhs.BaseId) < std::tie(acRhs.ModId, acRhs.BaseId);
}

//! The kinds held, sorted: one entry's count can be taken back by another entry of the same base form.
template <class TIsCounted> Vector<GameId> Held(const Inventory& acInventory, TIsCounted aIsCounted)
{
    Vector<std::pair<GameId, int64_t>> counts;
    for (const auto& entry : acInventory.Entries)
    {
        if (!aIsCounted(entry))
            continue;
        const auto it = std::find_if(counts.begin(), counts.end(), [&entry](const auto& acCount) { return acCount.first == entry.BaseId; });
        if (it == counts.end())
            counts.emplace_back(entry.BaseId, entry.Count);
        else
            it->second += entry.Count;
    }

    Vector<GameId> kinds;
    for (const auto& [id, count] : counts)
        if (count > 0)
            kinds.push_back(id);
    std::sort(kinds.begin(), kinds.end(), Before);
    return kinds;
}

//! Whether the copy keeps an owner's quest item as its own.
inline bool IsKeptQuestItem(const Inventory& acCopy, const GameId& acId) noexcept
{
    return std::any_of(
        acCopy.Entries.begin(), acCopy.Entries.end(), [&acId](const Inventory::Entry& acEntry) { return acEntry.IsQuestItem && acEntry.BaseId == acId && acEntry.Count > 0; });
}
} // namespace Detail

//! aIsItem: false for a form that is no item, such as a levelled list, which both carry from the base container.
//! The copy's own quest items are left out: setting its owner's inventory keeps them on purpose.
template <class TIsItem> Difference Compare(const Inventory& acOwner, const Inventory& acCopy, TIsItem aIsItem)
{
    const Vector<GameId> cOwner = Detail::Held(acOwner, [&aIsItem](const Inventory::Entry& acEntry) { return aIsItem(acEntry.BaseId); });
    const Vector<GameId> cCopy = Detail::Held(acCopy, [&aIsItem](const Inventory::Entry& acEntry) { return !acEntry.IsQuestItem && aIsItem(acEntry.BaseId); });

    Difference difference;
    std::set_difference(cCopy.begin(), cCopy.end(), cOwner.begin(), cOwner.end(), std::back_inserter(difference.Extra), Detail::Before);
    std::set_difference(cOwner.begin(), cOwner.end(), cCopy.begin(), cCopy.end(), std::back_inserter(difference.Missing), Detail::Before);

    // An owner's quest item the copy kept as its own is no difference either.
    difference.Missing.erase(
        std::remove_if(difference.Missing.begin(), difference.Missing.end(), [&acCopy](const GameId& acId) { return Detail::IsKeptQuestItem(acCopy, acId); }),
        difference.Missing.end());
    return difference;
}
} // namespace LootKinds
