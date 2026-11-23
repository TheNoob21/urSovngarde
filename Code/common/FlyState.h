#pragma once

#include <cstdint>

// A flyer's flight state, bits 18-20 of ActorState flags1 (CommonLib FLY_STATE).
// The game sets it only through Actor::SetFlyState (SE 36922, AE 37947), which also puts the character controller in or out of
// its flying state, and skips everything when the bits already hold the new state. So the bits are never written over raw:
// a copy given its owner's bits that way stayed on the ground controller, and fell when its game took it over in the air.
namespace FlyState
{
constexpr uint32_t kShift = 18;
constexpr uint32_t kMask = 0x7u << kShift;

enum : uint32_t
{
    kNone = 0,
    kTakeOff = 1,
    kCruising = 2,
    kHovering = 3,
    kLanding = 4,
    kPerching = 5,
    kAction = 6,
};

constexpr uint32_t Of(uint32_t aFlags1) noexcept
{
    return (aFlags1 & kMask) >> kShift;
}

//! In the air: anything but on the ground (none) or sitting on a perch.
constexpr bool InTheAir(uint32_t aState) noexcept
{
    return aState != kNone && aState != kPerching;
}

//! Whether a copy's flight state is set through the game (Actor::SetFlyState) to its owner's: a flyer, a dragon or a vampire
//! lord, with a movement controller (without one the setter does nothing), whose owner's state is not the copy's.
constexpr bool SetThroughGame(bool aFlyer, bool aHasController, uint32_t aOwnerFlags1, uint32_t aOwnFlags1) noexcept
{
    return aFlyer && aHasController && Of(aOwnerFlags1) != Of(aOwnFlags1);
}

//! A copy's flags1 from its owner's: the owner's, but the bits in aKeptMask and the flight state stay the copy's own.
constexpr uint32_t CopyFlags1(uint32_t aOwner, uint32_t aOwn, uint32_t aKeptMask) noexcept
{
    const uint32_t cKept = aKeptMask | kMask;
    return (aOwner & ~cKept) | (aOwn & cKept);
}
} // namespace FlyState
