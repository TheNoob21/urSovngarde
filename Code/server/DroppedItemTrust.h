#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>

//! What a player may add to the server's list of dropped items, how fast, and how many the server keeps.
//! A dropped item is kept for good and sent to everyone who comes near, so a player far from a place must not add or remove one there.
//! Pure rules on plain values, so they can be tested (tests/dropped_item_trust.cpp).
namespace DroppedItemTrust
{
//! Items kept at most. Nothing expires a dropped item, so a long game fills the list slowly; when it is full the oldest goes.
constexpr size_t kMaxItems = 16384;

//! A game announces every item of its earlier drops it has loaded, up to a thousand, in one burst after a load.
constexpr double kBurst = 1536.0;
//! Adds a second a player earns back once the burst is used. Honest drops come far slower.
constexpr double kPerSecond = 16.0;

//! Whether a player may add an item at a place.
//! aAnnouncement: the game reports an item that already lies there (from an earlier drop), instead of a drop it has just seen.
//! aHasCell: the server already knows which cell the sender stands in; aCellInRange: that cell's loaded grid holds the item's cell.
//! aNear: the sender's character is near the place (MoverNearness).
//! A new drop needs the sender beside it. An announcement is sent as the game connects and may arrive before the game's first cell
//! entry does, so it needs only the grid once the cell is known: what a game announces lies in the cells it has loaded.
inline bool MayAdd(const bool aAnnouncement, const bool aHasCell, const bool aCellInRange, const bool aNear) noexcept
{
    if (aAnnouncement)
        return !aHasCell || aCellInRange;
    return aNear;
}

//! Whether a player may remove an item: only one near where the server has it, as for a move.
inline bool MayRemove(const bool aNear) noexcept
{
    return aNear;
}

//! How many adds a player may make: a burst, then a steady few a second. One for each player.
struct AddBudget
{
    using Clock = std::chrono::steady_clock;

    double Tokens{kBurst};
    Clock::time_point Last{};

    bool Allow(const Clock::time_point aNow) noexcept
    {
        if (Last != Clock::time_point{})
        {
            const double cSeconds = std::chrono::duration<double>(aNow - Last).count();
            if (cSeconds > 0.0)
                Tokens = std::min(kBurst, Tokens + cSeconds * kPerSecond);
        }
        Last = aNow;
        if (Tokens < 1.0)
            return false;
        Tokens -= 1.0;
        return true;
    }
};

//! Makes room for one more item in a map with ids counting up, at most aMax: the oldest (lowest id) go. Returns how many went.
template <class TMap> size_t ForgetOldest(TMap& aItems, const size_t aMax) noexcept
{
    size_t cForgotten = 0;
    while (!aItems.empty() && aItems.size() >= aMax)
    {
        aItems.erase(aItems.begin());
        ++cForgotten;
    }
    return cForgotten;
}
} // namespace DroppedItemTrust
