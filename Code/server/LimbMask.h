#pragma once

#include <cstdint>

//! The limbs cut off an actor, one bit per limb as the engine numbers them, kept by the server beside whether it is dead.
//! Plain values in and out, so the unit tests check the rule without a server (CharacterService applies it).
namespace LimbMask
{
//! A limb once cut off stays cut off: what a game reports is added to what the server keeps.
inline uint16_t Merge(const uint16_t aKept, const uint16_t aReported) noexcept
{
    return static_cast<uint16_t>(aKept | aReported);
}
} // namespace LimbMask
