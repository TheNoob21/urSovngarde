#pragma once

#include <cstdint>

// Whether an actor sneaks: bit 9 of ActorState flags1 (CommonLib ActorState1 sneaking), what the game's detection reads.
// A copy gets its owner's flags1 with each replayed action, taken as the owner's game was about to perform it. For the sneak
// toggle (Skyrim.esm ActionSneak, the same form on SE and VR) that is the state before the toggle, so the copy stood while its
// owner sneaked, until some later action; a player who sends few actions, as in VR, could stay "standing" there for long.
// Only actions the owner's game performed are sent, so after ActionSneak the owner sneaks unless it did before.
namespace SneakState
{
constexpr uint32_t kBit = 1u << 9;
constexpr uint32_t kActionSneak = 0x13007;

constexpr bool Of(uint32_t aFlags1) noexcept
{
    return (aFlags1 & kBit) != 0;
}

//! A copy's flags1 after it replayed an action: its sneak bit as its owner's after that action, the rest left as it is.
constexpr uint32_t AfterAction(uint32_t aActionId, uint32_t aOwnerBefore, uint32_t aCopy) noexcept
{
    if (aActionId != kActionSneak)
        return aCopy;
    return Of(aOwnerBefore) ? (aCopy & ~kBit) : (aCopy | kBit);
}
} // namespace SneakState
