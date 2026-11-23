#pragma once

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

//! A rider's copy on its horse is held where the horse is drawn this frame, not where its own updates put it: rider and horse
//! updates arrive on their own timing and their owner's game puts its rider on the horse a frame apart, so taking the rider from
//! its own updates moved it about the saddle, more the faster the horse went. Headings are the game's:
//! 0 faces +y, and a quarter turn faces +x.
namespace SeatPin
{
//! A seat further than this from the horse's own place is no seat: the rider is held on the horse's place itself.
inline constexpr float kMaxSeatOffset = 64.f;
//! A seat offset found after the copy was held on the horse's place is eased in over this long.
inline constexpr double kSeatEaseMs = 150.0;
//! The seat is taken once its owner's rider has stayed within this many units of one place on the horse for kSeatSettledMs
//! (its updates play a little behind, so the first ones after the copy sits still show the rider climbing), or kSeatLatestMs
//! after the copy sat, whatever it is then.
inline constexpr float kSeatSettleUnits = 2.f;
inline constexpr double kSeatSettledMs = 200.0;
inline constexpr double kSeatLatestMs = 1500.0;

//! Whether the seat is taken now: steady for long enough, or the copy sat long enough ago.
constexpr bool SeatSettled(const double aSteadyForMs, const double aSeatedForMs) noexcept
{
    return aSteadyForMs >= kSeatSettledMs || aSeatedForMs >= kSeatLatestMs;
}

//! Whether a horse's place and heading can hold a rider (a number, not a broken reading).
inline bool Holds(const glm::vec3& acHorse, const float aHeading) noexcept
{
    return std::isfinite(acHorse.x) && std::isfinite(acHorse.y) && std::isfinite(acHorse.z) && std::isfinite(aHeading);
}

//! Use the horse's clock while the pair is drawn together.
inline double PlaybackTick(const bool aPaired, const double aMountTick, const double aRiderTick) noexcept
{
    return aPaired && aMountTick > 0.0 ? aMountTick : aRiderTick;
}

//! The climb uses the saddle heading. A rider in the saddle can turn independently of the horse.
inline float RiderHeading(const float aHorse, const float aRider, const bool aSeated, const bool aSampled) noexcept
{
    return aSeated && aSampled && std::isfinite(aRider) ? aRider : aHorse;
}

//! A world offset from the horse, as seen from the horse (x to its right, y ahead of it, z up).
inline glm::vec3 ToHorse(const glm::vec3& acWorld, const float aHeading) noexcept
{
    const float cCos = std::cos(aHeading);
    const float cSin = std::sin(aHeading);
    return {acWorld.x * cCos - acWorld.y * cSin, acWorld.x * cSin + acWorld.y * cCos, acWorld.z};
}

//! An offset as seen from the horse, in the world.
inline glm::vec3 FromHorse(const glm::vec3& acLocal, const float aHeading) noexcept
{
    const float cCos = std::cos(aHeading);
    const float cSin = std::sin(aHeading);
    return {acLocal.x * cCos + acLocal.y * cSin, -acLocal.x * cSin + acLocal.y * cCos, acLocal.z};
}

//! The seat, as seen from the horse, from where the rider's owner had the rider and the horse at the same instant. None when
//! that is too far to be a seat, or not a number.
inline glm::vec3 Capture(const glm::vec3& acRider, const glm::vec3& acHorse, const float aHeading) noexcept
{
    const glm::vec3 cLocal = ToHorse(acRider - acHorse, aHeading);
    const float cLength = glm::length(cLocal);
    return std::isfinite(cLength) && cLength <= kMaxSeatOffset ? cLocal : glm::vec3{};
}

//! How much of a seat found aSinceMs ago is applied: none at first, all of it from kSeatEaseMs on, easing in and out.
inline float SeatWeight(const double aSinceMs) noexcept
{
    if (!(aSinceMs > 0.0))
        return 0.f;
    if (aSinceMs >= kSeatEaseMs)
        return 1.f;
    const double cShare = aSinceMs / kSeatEaseMs;
    return static_cast<float>(cShare * cShare * (3.0 - 2.0 * cShare));
}

//! Where the rider's copy is held: the horse's place this frame and the seat turned with the horse.
inline glm::vec3 Place(const glm::vec3& acHorse, const float aHeading, const glm::vec3& acSeat) noexcept
{
    return acHorse + FromHorse(acSeat, aHeading);
}

//! Where a rider's copy placed at the frame end goes: on its horse as the horse stands then, which was placed after the rider's
//! own step had read it, or where that step put it when the horse cannot hold it (gone, or not a number).
inline glm::vec3 FrameEndPlace(const bool aHorseThere, const glm::vec3& acHorse, const float aHeading, const glm::vec3& acSeat, const glm::vec3& acStepPlace) noexcept
{
    return aHorseThere && Holds(acHorse, aHeading) ? Place(acHorse, aHeading, acSeat) : acStepPlace;
}

//! A copy let off its horse glides to where its owner has it, never below that place: its owner's rider stands on the ground
//! there, and a glide from the saddle that dipped under it showed the body in the floor for a moment.
inline glm::vec3 AboveGround(const glm::vec3& acGlide) noexcept
{
    return {acGlide.x, acGlide.y, std::max(acGlide.z, 0.f)};
}
} // namespace SeatPin
