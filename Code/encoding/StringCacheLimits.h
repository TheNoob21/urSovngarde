#pragma once

#include <cstddef>

//! Which strings the shared string cache takes up. The cache only grows, every name in it is sent to every game, and its ids are
//! 16 bits on the wire, so a game that sent endless new names would fill it and stop every game's names from resolving.
//! A name the cache refuses is still sent whole in each message, so nothing is lost but the saving.
namespace StringCacheLimits
{
//! Names kept at most, well below what the ids can count.
constexpr size_t kMaxEntries = 32768;
//! The longest name that is kept; a longer one is sent whole each time.
constexpr size_t kLongestName = 256;

//! aEntries: names in the cache; aWanted: names waiting to go in; aLength: of this one; aKnown: it is in the cache already.
inline bool MayWant(const size_t aEntries, const size_t aWanted, const size_t aLength, const bool aKnown) noexcept
{
    if (aKnown)
        return true;
    return aLength <= kLongestName && aEntries + aWanted < kMaxEntries;
}
} // namespace StringCacheLimits
