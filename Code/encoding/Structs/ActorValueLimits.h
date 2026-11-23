#pragma once

#include <cstdint>

//! The most actor value entries a message holds, and the largest key a game knows. A list read from the network is cut to these:
//! a crafted count must not keep the reader looping, and an unknown key must not reach the game.
namespace ActorValueLimits
{
//! One entry per actor value the game has; a real message never holds more.
constexpr uint64_t kMaxEntries = 256;
//! The number of actor values the game defines; a key at or above it names none of them.
constexpr uint64_t kKeyLimit = 164;

//! True when a key read from the network names an actor value of the game.
inline bool IsKnownKey(const uint64_t aKey) noexcept
{
    return aKey < kKeyLimit;
}
} // namespace ActorValueLimits
