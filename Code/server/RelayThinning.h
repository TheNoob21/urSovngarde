#pragma once

#include <cstdint>
#include <optional>

//! How many of an actor's movement updates the server passes on to a player, by how far that player stands from it.
//! Every player in range got all of them, 30 a second, even 100 m away, so the server's upload grew with the square of the players.
//! A near player still gets every one.
namespace RelayThinning
{
//! About 40 m, and about 100 m.
constexpr float kMidDistance = 2800.f;
constexpr float kFarDistance = 7000.f;
//! On the owner's clock: about every second update of 30 a second beyond the first distance, every fourth beyond the second.
constexpr uint64_t kMidIntervalMs = 60;
constexpr uint64_t kFarIntervalMs = 120;

enum class Band : uint8_t
{
    kNear,
    kMid,
    kFar
};

//! A player elsewhere, or with no known place, counts as near: nothing is held back from it.
inline Band BandOf(const std::optional<float> aDistance) noexcept
{
    if (!aDistance || *aDistance <= kMidDistance)
        return Band::kNear;
    return *aDistance <= kFarDistance ? Band::kMid : Band::kFar;
}

//! Whether an update at aTick is due for a band last sent the actor's update at aLastTick (0: never).
//! A tick going back is another owner's clock, after a hand-over: due.
inline bool IsDue(const uint64_t aTick, const uint64_t aLastTick, const uint64_t aIntervalMs) noexcept
{
    return aLastTick == 0 || aTick < aLastTick || aTick - aLastTick >= aIntervalMs;
}

//! aCarriesKeptParts: the update carries a part sent only on change or once a second (fingers, scale, weapons, hand check),
//! which the receiver keeps until the next; it always goes, or a far player might never get one.
inline bool Relays(const Band aBand, const bool aMidDue, const bool aFarDue, const bool aCarriesKeptParts) noexcept
{
    switch (aBand)
    {
    case Band::kNear: return true;
    case Band::kMid: return aMidDue || aCarriesKeptParts;
    case Band::kFar: return aFarDue || aCarriesKeptParts;
    }
    return true;
}
} // namespace RelayThinning
