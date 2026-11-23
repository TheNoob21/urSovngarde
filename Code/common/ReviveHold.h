#pragma once

#include <cstdint>

// Which downed player a reviver's hand is on, with no game in it.
namespace ReviveHold
{
// The downed player a hold is on (0 for none) stays the target while a hand still touches them, though another downed player
// earlier in the list is touched too: the hold is seconds long, and starting over on every switch would never end it.
// Otherwise the first downed player touched is, or nobody (0).
inline uint32_t Target(const uint32_t aHeld, const bool aHeldTouched, const uint32_t aFirstTouched) noexcept
{
    return aHeld && aHeldTouched ? aHeld : aFirstTouched;
}
} // namespace ReviveHold
