#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

//! An arrow stuck in a wall or the ground, taken by one player, goes in every game.
//!
//! Every arrow is replayed as a real arrow in the other games, so it sticks where it lands there too: each game has its own
//! copy of it, with an id only that game knows. A player taking it out of the wall gets it back in their game only, and the
//! other player could take theirs too, a free arrow. So a taken arrow is told by what it is (its projectile) and where it
//! was, and every other game removes its own copy of the same projectile nearest that place, if one is close enough.
namespace ArrowPickup
{
//! How far another game's copy of the arrow may be from where it was taken (units): a flight that differs a little.
inline constexpr float kMatchUnits = 30.f;
//! How far from the player's last known place the server takes a pick-up (units): an arm's reach, a VR arm and a late
//! position report.
inline constexpr float kReachUnits = 512.f;
//! Pick-ups a player may send in a second; more are refused.
inline constexpr uint32_t kMaxPerSecond = 8;
//! The world's coordinates are never larger than this (as ClientTrust::kMaxCoordinate).
inline constexpr float kMaxCoordinate = 1.0e7f;

//! A reference in this game that may be its copy of the taken arrow.
struct Candidate
{
    float Distance{};      //!< from where the other player took theirs
    bool SameProjectile{}; //!< its base form is the taken arrow's projectile
    bool Temporary{};      //!< made at run time, as a shot arrow is (never a placed reference of a plugin)
    bool OnActor{};        //!< stuck in or lying with an actor or a body: StuckArrows shares those, never this
    bool AtRest{};         //!< landed or stuck: it did not move between two reads (IsAtRest); an arrow in flight never matches
};

//! Whether a candidate may be removed for the taken arrow.
inline bool Matches(const Candidate& acCandidate) noexcept
{
    return acCandidate.SameProjectile && acCandidate.Temporary && !acCandidate.OnActor && acCandidate.AtRest && std::isfinite(acCandidate.Distance) &&
           acCandidate.Distance <= kMatchUnits;
}

//! A candidate is read twice, at least this long apart (ms), and is at rest when it moved no more than kRestUnits in between.
//! An arrow in flight covers hundreds of units in that time; one stuck in a wall or lying on the ground does not move.
inline constexpr uint64_t kRestWaitMs = 100;
inline constexpr float kRestUnits = 1.f;

//! Whether the second read is due, the first at aFirstMs (a clock that stepped back makes it due at once).
inline bool RestReadDue(const uint64_t aFirstMs, const uint64_t aNowMs) noexcept
{
    return aNowMs < aFirstMs || aNowMs - aFirstMs >= kRestWaitMs;
}

inline bool IsAtRest(const float aX0, const float aY0, const float aZ0, const float aX1, const float aY1, const float aZ1) noexcept
{
    const float cX = aX1 - aX0;
    const float cY = aY1 - aY0;
    const float cZ = aZ1 - aZ0;
    const float cSquared = cX * cX + cY * cY + cZ * cZ;
    return std::isfinite(cSquared) && cSquared <= kRestUnits * kRestUnits;
}

//! Of the candidates, the one to remove: the nearest that Matches, or aCount when none does.
inline size_t PickNearest(const Candidate* apCandidates, const size_t aCount) noexcept
{
    size_t best = aCount;
    float bestDistance = std::numeric_limits<float>::max();
    for (size_t i = 0; i < aCount; ++i)
    {
        if (!Matches(apCandidates[i]) || apCandidates[i].Distance >= bestDistance)
            continue;
        best = i;
        bestDistance = apCandidates[i].Distance;
    }
    return best;
}

//! An arrow this close to an actor's place counts as stuck in it or lying with its body (units): within its width around
//! it, from a little below its feet to above its head. A wall arrow right next to an NPC is left too; that is the safe side.
inline constexpr float kActorRadius = 64.f;
inline constexpr float kActorBelow = 32.f;
inline constexpr float kActorAbove = 200.f;

inline bool IsWithActor(const float aX, const float aY, const float aZ, const float aActorX, const float aActorY, const float aActorZ) noexcept
{
    const float cX = aX - aActorX;
    const float cY = aY - aActorY;
    return cX * cX + cY * cY <= kActorRadius * kActorRadius && aZ >= aActorZ - kActorBelow && aZ <= aActorZ + kActorAbove;
}

//! Whether an actor at that place keeps the arrow out of the pick-up. A player never does, this game's own or another
//! player's character: the one taking the arrow stands right next to it, in every game. An arrow in a player is not a
//! loose arrow either; it is part of that player's 3D, never a reference to pick up.
inline bool KeptForActor(const float aX, const float aY, const float aZ, const float aActorX, const float aActorY, const float aActorZ, const bool aActorIsPlayer) noexcept
{
    return !aActorIsPlayer && IsWithActor(aX, aY, aZ, aActorX, aActorY, aActorZ);
}

//! Outdoors, the arrow is looked for in the cell under its place and the eight around it: near a cell's edge, another
//! game's copy may lie in the neighbouring cell. Exterior cells are 4096 units a side.
inline constexpr float kCellUnits = 4096.f;

inline int32_t GridOf(const float aCoordinate) noexcept
{
    return static_cast<int32_t>(std::floor(aCoordinate / kCellUnits));
}

enum class Refusal : uint8_t
{
    kNone,
    kNotAPlace, // a coordinate that is not a usable number
    kNoPicker,  // the server knows no place of the player's character
    kTooFar,    // further from the player than an arm's reach
    kTooOften,  // more than kMaxPerSecond in a second
    kNoArrow,   // no projectile named
    kWrongCell, // indoors, not the player's own cell; outdoors, not in the player's grid cell or one around it
};

inline const char* Describe(const Refusal aRefusal) noexcept
{
    switch (aRefusal)
    {
    case Refusal::kNone: return "taken";
    case Refusal::kNotAPlace: return "its place is not a usable number";
    case Refusal::kNoPicker: return "the player's character has no known place";
    case Refusal::kTooFar: return "it is further from the player than an arm's reach";
    case Refusal::kTooOften: return "the player sent too many in a second";
    case Refusal::kNoArrow: return "no projectile was named";
    case Refusal::kWrongCell: return "it is not in the player's cell (indoors) or the cells around the player (outdoors)";
    }
    return "unknown";
}

inline bool IsPlace(const float aX, const float aY, const float aZ) noexcept
{
    return std::isfinite(aX) && std::isfinite(aY) && std::isfinite(aZ) && std::fabs(aX) <= kMaxCoordinate && std::fabs(aY) <= kMaxCoordinate && std::fabs(aZ) <= kMaxCoordinate;
}

//! A player's pick-ups in the current second.
struct RateGate
{
    uint64_t WindowStartMs{};
    uint32_t Count{};

    //! Whether one more pick-up at aNowMs is allowed; counts it when it is.
    bool Allow(const uint64_t aNowMs) noexcept
    {
        if (aNowMs < WindowStartMs || aNowMs - WindowStartMs >= 1000)
        {
            WindowStartMs = aNowMs;
            Count = 0;
        }
        if (Count >= kMaxPerSecond)
            return false;
        ++Count;
        return true;
    }
};

struct Check
{
    bool HasArrow{};     // a projectile was named
    float X{}, Y{}, Z{}; // where it was taken
    bool HasPicker{};    // the server knows where the player's character is
    float PickerX{}, PickerY{}, PickerZ{};
    bool PickerInterior{}; // the player's character is in an interior cell
    bool SameCell{};       // the message's cell is the player's character's cell
    int32_t PickerGridX{}; // outdoors, the grid cell the player's character is in
    int32_t PickerGridY{};
};

//! Whether the server relays a pick-up (the rate is checked apart, by RateGate, once the rest passes).
inline Refusal Judge(const Check& acCheck) noexcept
{
    if (!acCheck.HasArrow)
        return Refusal::kNoArrow;
    if (!IsPlace(acCheck.X, acCheck.Y, acCheck.Z))
        return Refusal::kNotAPlace;
    if (!acCheck.HasPicker || !IsPlace(acCheck.PickerX, acCheck.PickerY, acCheck.PickerZ))
        return Refusal::kNoPicker;
    const float cX = acCheck.X - acCheck.PickerX;
    const float cY = acCheck.Y - acCheck.PickerY;
    const float cZ = acCheck.Z - acCheck.PickerZ;
    if (cX * cX + cY * cY + cZ * cZ > kReachUnits * kReachUnits)
        return Refusal::kTooFar;
    // Indoors the arrow is in the player's own cell. Outdoors another game's copy near a cell's edge may lie in the neighbouring
    // cell, so the arrow's place must be in the player's grid cell or one of the eight around it.
    if (acCheck.PickerInterior)
    {
        if (!acCheck.SameCell)
            return Refusal::kWrongCell;
    }
    else
    {
        const int32_t cDX = GridOf(acCheck.X) - acCheck.PickerGridX;
        const int32_t cDY = GridOf(acCheck.Y) - acCheck.PickerGridY;
        if (cDX < -1 || cDX > 1 || cDY < -1 || cDY > 1)
            return Refusal::kWrongCell;
    }
    return Refusal::kNone;
}
} // namespace ArrowPickup
