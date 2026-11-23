#pragma once

#include <Structs/GameId.h>
#include <Structs/Inventory.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <tuple>

// Two players can take the same item at once: the last of a stack in a chest, a dropped item, a sword on a table.
// Each game gives it to its own player at once, so the server takes the first take it hears of and the later one is lost:
// that player's game takes the item back off its player (NotifyItemTakeLost).
// Plain values in and out, so the unit tests check it without a server.
namespace ItemTakes
{
using Clock = std::chrono::steady_clock;

//! How long a take stays its player's: far longer than two takes can cross on the network, and than a menu stays open.
inline constexpr auto kHeld = std::chrono::minutes(10);

//! Who took each thing last, for kHeld.
template <class Key> class Claims
{
public:
    //! A long session's worth of takes. Lapsed ones go first, then the oldest.
    static constexpr size_t kMax = 4096;

    //! True when the thing is this player's to take: nobody else took it lately.
    bool Claim(const Key& acKey, const uint32_t aPlayerId, const Clock::time_point aNow) noexcept
    {
        if (TakenByAnother(acKey, aPlayerId, aNow))
            return false;
        Note(acKey, aPlayerId, aNow);
        return true;
    }

    [[nodiscard]] bool TakenByAnother(const Key& acKey, const uint32_t aPlayerId, const Clock::time_point aNow) const noexcept
    {
        const auto it = m_takes.find(acKey);
        return it != m_takes.end() && it->second.PlayerId != aPlayerId && aNow - it->second.At < kHeld;
    }

    //! This player took it now, whoever took it before.
    void Note(const Key& acKey, const uint32_t aPlayerId, const Clock::time_point aNow) noexcept
    {
        if (m_takes.size() >= kMax && !m_takes.contains(acKey))
            Prune(aNow);
        m_takes[acKey] = Take{aPlayerId, aNow};
    }

    void Clear() noexcept { m_takes.clear(); }
    [[nodiscard]] size_t Size() const noexcept { return m_takes.size(); }

private:
    void Prune(const Clock::time_point aNow) noexcept
    {
        std::erase_if(m_takes, [aNow](const auto& acTake) { return aNow - acTake.second.At >= kHeld; });
        if (m_takes.size() < kMax)
            return;
        const auto cOldest = std::min_element(m_takes.begin(), m_takes.end(), [](const auto& acLhs, const auto& acRhs) { return acLhs.second.At < acRhs.second.At; });
        m_takes.erase(cOldest);
    }

    struct Take
    {
        uint32_t PlayerId{};
        Clock::time_point At{};
    };
    std::map<Key, Take> m_takes;
};

//! One item in one container or body: its server id, and the item's base.
using StackKey = std::tuple<uint32_t, uint32_t, uint32_t>;
inline StackKey Stack(const uint32_t aServerId, const GameId& acBaseId) noexcept
{
    return {aServerId, acBaseId.ModId, acBaseId.BaseId};
}

//! A placed reference, by its id.
inline uint64_t Placed(const GameId& acObjectId) noexcept
{
    return acObjectId.LogFormat();
}

//! How much of a take out of a container or a body the server gives.
struct Split
{
    int32_t Given{};
    int32_t Refused{};
};

//! aHeld: how many the server holds; aWanted: how many the game took; aRaced: another player took the same item from it lately.
//! Only a race refuses: a shortfall nobody else made is the server's copy lagging behind the game's, so that take stands.
//! A quest item is never refused, as each player's quest needs its own.
inline Split Decide(const int32_t aHeld, const int32_t aWanted, const bool aRaced, const bool aQuestItem) noexcept
{
    if (aWanted <= 0)
        return {};
    const int32_t cHeld = std::max(aHeld, 0);
    if (aWanted <= cHeld || !aRaced || aQuestItem)
        return {aWanted, 0};
    return {cHeld, aWanted - cHeld};
}

//! A take marks its stack as its player's only for what the server held: one past it is the server's copy lagging the games',
//! which another game's take of the same item shares.
inline bool MarksStack(const Split& acSplit, const int32_t aHeld) noexcept
{
    return std::min(acSplit.Given, aHeld) > 0;
}

//! How many of an item an inventory holds, whatever their extra data.
//! The server's copy can differ from the game's in a float of an item's health or charge, which is still the same item.
inline int32_t Held(const Inventory& acInventory, const GameId& acBaseId) noexcept
{
    int32_t held = 0;
    for (const Inventory::Entry& cEntry : acInventory.Entries)
    {
        if (cEntry.BaseId == acBaseId && cEntry.Count > 0)
            held += cEntry.Count;
    }
    return held;
}

//! Takes up to aCount of the item out, the stack it merges with first. A count never goes below zero.
inline void Remove(Inventory& aInventory, const Inventory::Entry& acItem, int32_t aCount) noexcept
{
    auto& entries = aInventory.Entries;
    const auto cTakeFrom = [&aCount](Inventory::Entry& aEntry)
    {
        const int32_t cTaken = std::min(aCount, std::max(aEntry.Count, 0));
        aEntry.Count -= cTaken;
        aCount -= cTaken;
    };

    for (Inventory::Entry& entry : entries)
    {
        if (aCount > 0 && entry.CanBeMerged(acItem))
            cTakeFrom(entry);
    }
    for (Inventory::Entry& entry : entries)
    {
        if (aCount > 0 && entry.BaseId == acItem.BaseId)
            cTakeFrom(entry);
    }

    std::erase_if(entries, [&acItem](const Inventory::Entry& acEntry) { return acEntry.BaseId == acItem.BaseId && acEntry.Count <= 0; });
}
} // namespace ItemTakes
