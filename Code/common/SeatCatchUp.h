#pragma once

#include <NpcFurniture.h>

#include <cstdint>

//! What the server tells a player it sends an NPC to later: the seat its owner's game said last, so that the copy sits at once
//! instead of standing in its chair until the next word.
namespace SeatCatchUp
{
//! Told only while settled, under the epoch the server holds, and said again lately (a copy lets go of a seat older than
//! NpcFurniture::kExpireMs, so an older one would only seat it for a moment).
constexpr bool Tell(const uint8_t aState, const uint32_t aRelayedEpoch, const uint32_t aHeldEpoch, const bool aHasFurniture, const uint64_t aSinceRelayedMs) noexcept
{
    return (aState == NpcFurniture::kStateSitting || aState == NpcFurniture::kStateSleeping) && aHasFurniture && aRelayedEpoch != 0 && aRelayedEpoch == aHeldEpoch &&
           aSinceRelayedMs < NpcFurniture::kExpireMs;
}
} // namespace SeatCatchUp
