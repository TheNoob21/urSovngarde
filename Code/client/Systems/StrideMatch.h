#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iosfwd>
#include <optional>

#include <glm/glm.hpp>

#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <Structs/AnimationVariables.h>

//! How fast a copy whose movement comes from another player's game moves its legs: at the speed it is shown moving, not at what
//! its owner's graph said.
//! The game picks the walk, run or sprint-fast clip and its playback rate from the graph's sampled speed alone, which it works
//! out from "Speed" for its own clips, so a copy whose Speed is the speed it is drawn moving has its feet keep pace with it.
namespace StrideMatch
{
//! Ignore tiny position changes when the copy is standing.
inline constexpr float kStillSpeed = 10.f;
//! Faster than this between two points is a snap, not a stride.
inline constexpr float kMaxStrideSpeed = 2000.f;
//! The shown speed eases toward each new pair of points over about this long, so one early or late point does not pump the legs.
inline constexpr double kEaseMs = 120.0;
//! A sprint shown slower than this is not one. The sprint clip plays at a fixed rate and moves its feet at 370 units a second;
//! the game's own sprint is 500 (NPC_Sprinting_MT) and its run 370 (NPC_Default_MT), so a body shown slower than its sprint
//! clip has fallen below a run: its feet would run ahead of it, and the run's clips have a matching pace at every speed.
inline constexpr float kSprintFloor = 370.f;
//! For this long before the copy's sprint is ended, so the speed-up at the start of a sprint is left alone.
inline constexpr double kSprintSlowMs = 400.0;
//! A copy shown this fast moves at a sprint's pace (the game's own sprint is 500): one whose sprint was ended by a slow spell (a
//! turn, a bump) and that is back at this pace sprints again. Between kSprintFloor and this it stays as it is, so it does not
//! flick between the two.
inline constexpr float kSprintStartPace = 450.f;
//! The copy's legs and its owner's graph disagree when they are this far apart, and this much apart as a share.
inline constexpr float kMismatchUnits = 40.f;
inline constexpr float kMismatchShare = 0.3f;

//! Whether an actor's legs are matched to the speed it is shown moving: its movement comes from another player's
//! game, it is not a mount (a horse has its own sampled speed and its rider's seat), it stands on its feet, and its graph is the
//! humanoid one (0_master), whose walk and run blends are all driven by the speed it samples from Speed.
constexpr bool Applies(const bool aRemote, const bool aMount, const bool aOnFoot, const bool aHumanoid) noexcept
{
    return aRemote && !aMount && aOnFoot && aHumanoid;
}

inline std::optional<float> GroundSpeed(const glm::vec3& acFrom, const glm::vec3& acTo, const double aFrameMs, const bool aReset = false) noexcept
{
    if (aReset || !(aFrameMs > 0.0 && aFrameMs <= 250.0))
        return std::nullopt;
    const float cSpeed = glm::length(glm::vec2{acTo.x - acFrom.x, acTo.y - acFrom.y}) * 1000.f / static_cast<float>(aFrameMs);
    if (!std::isfinite(cSpeed) || cSpeed > kMaxStrideSpeed)
        return std::nullopt;
    return cSpeed;
}

//! The shown speed eased toward aSample over aDeltaMs. A negative aShown is none yet: the sample is taken whole.
inline float Ease(const float aShown, const float aSample, const double aDeltaMs) noexcept
{
    if (!std::isfinite(aSample))
        return aShown;
    if (!(aShown >= 0.f) || !std::isfinite(aShown))
        return aSample;
    if (!(aDeltaMs > 0.0))
        return aShown;
    const float cShare = static_cast<float>(1.0 - std::exp(-aDeltaMs / kEaseMs));
    return aShown + (aSample - aShown) * cShare;
}

//! Use the owner's graph before a copy has a measured stride.
inline float LegSpeed(const float aShown, const float aOwnerSpeed) noexcept
{
    const float cOwner = std::isfinite(aOwnerSpeed) ? std::max(aOwnerSpeed, 0.f) : 0.f;
    if (!(aShown >= 0.f) || !std::isfinite(aShown))
        return cOwner;
    return aShown >= kStillSpeed ? aShown : 0.f;
}

//! Writes aSpeed into the Speed and sampled-speed slots of aVariables. A slot that is negative or past the list is left alone.
inline void Drive(AnimationVariables& aVariables, const int aSpeedSlot, const int aSampledSlot, const float aSpeed) noexcept
{
    for (const int cSlot : {aSpeedSlot, aSampledSlot})
        if (cSlot >= 0 && static_cast<size_t>(cSlot) < aVariables.Floats.size())
            aVariables.Floats[cSlot] = aSpeed;
}

//! A full turn in radians.
inline constexpr float kFullTurn = 6.28318530718f;

//! Which way a step goes against a heading, as the graph's "Direction" has it: a share of a full turn clockwise from the
//! heading, 0 forward, 0.25 right, 0.5 back, 0.75 left. aHeading is the game's (radians, 0 along +y, turning towards +x).
//! None for a step with no length across the ground.
inline std::optional<float> StepDirection(const glm::vec3& acFrom, const glm::vec3& acTo, const float aHeading) noexcept
{
    const float cX = acTo.x - acFrom.x;
    const float cY = acTo.y - acFrom.y;
    if (!std::isfinite(cX) || !std::isfinite(cY) || !std::isfinite(aHeading) || cX * cX + cY * cY < 1e-6f)
        return std::nullopt;
    float share = (std::atan2(cX, cY) - aHeading) / kFullTurn;
    share -= std::floor(share);
    // A hair below 0 lands on 1, the same way.
    return share < 1.f ? share : 0.f;
}

//! The shown direction eased toward aSample over aDeltaMs, the short way round. A negative aShown is none yet: the sample is
//! taken whole.
inline float EaseDirection(const float aShown, const float aSample, const double aDeltaMs) noexcept
{
    if (!std::isfinite(aSample) || aSample < 0.f || aSample >= 1.f)
        return aShown;
    if (!(aShown >= 0.f) || !(aShown < 1.f))
        return aSample;
    if (!(aDeltaMs > 0.0))
        return aShown;
    const float cShare = static_cast<float>(1.0 - std::exp(-aDeltaMs / kEaseMs));
    float turn = aSample - aShown;
    turn -= std::round(turn);
    float direction = aShown + turn * cShare;
    direction -= std::floor(direction);
    return direction < 1.f ? direction : 0.f;
}

//! A graph direction as the actor's process has it: radians from -pi to pi, 0 forward, pi/2 right.
inline float DirectionRadians(const float aShare) noexcept
{
    float radians = aShare * kFullTurn;
    if (radians > kFullTurn / 2.f)
        radians -= kFullTurn;
    return radians;
}

//! Whether the legs' speed and the owner's graph speed are far enough apart to say so.
inline bool Mismatch(const float aLegs, const float aOwner) noexcept
{
    const float cGap = std::fabs(aLegs - aOwner);
    return cGap > kMismatchUnits && cGap > kMismatchShare * std::max(aLegs, aOwner);
}

//! For this long a disagreement lasts before it is said: the legs ease, so every start and stop disagrees for a moment.
inline constexpr double kMismatchSayMs = 500.0;

//! Since when the legs and the owner's graph have disagreed, and whether that spell was said.
struct MismatchWatch
{
    double Since{-1.0};
    bool Said{false};
};

//! Whether a disagreement is to be said now: once per spell, when it has lasted kMismatchSayMs.
inline bool SayMismatch(MismatchWatch& aWatch, const bool aMismatch, const double aNow) noexcept
{
    if (!aMismatch)
    {
        aWatch = {};
        return false;
    }
    if (aWatch.Since < 0.0)
        aWatch.Since = aNow;
    if (aWatch.Said || aNow - aWatch.Since < kMismatchSayMs)
        return false;
    aWatch.Said = true;
    return true;
}

//! Since when a sprinting copy has been shown slower than a sprint; negative while it is not. Since when a copy not sprinting has
//! been shown at a sprint's pace, and whether its sprint was started in this climb to that pace.
struct SprintWatch
{
    double SlowSince{-1.0};
    double FastSince{-1.0};
    bool Started{false};
};

//! Whether the copy's sprint is ended now: its graph sprints, and it has been shown slower than kSprintFloor for kSprintSlowMs.
//! Once it says so the watch starts again, so the stop is asked for once.
inline bool StopsSprint(SprintWatch& aWatch, const bool aSprinting, const float aShown, const double aNow) noexcept
{
    if (!aSprinting || !(aShown < kSprintFloor))
    {
        aWatch.SlowSince = -1.0;
        return false;
    }
    if (aWatch.SlowSince < 0.0)
    {
        aWatch.SlowSince = aNow;
        return false;
    }
    if (aNow - aWatch.SlowSince < kSprintSlowMs)
        return false;
    aWatch.SlowSince = -1.0;
    return true;
}

//! Whether the copy's sprint is started now: its graph does not sprint, and it has been shown at kSprintStartPace or faster for
//! kSprintSlowMs. Once per climb: not again until it has been shown slower than kSprintFloor, so a start its graph refused is not
//! sent over and over. The owner's own replayed SprintStart starts a sprint as it begins; this only brings one back.
inline bool StartsSprint(SprintWatch& aWatch, const bool aSprinting, const float aShown, const double aNow) noexcept
{
    if (!(aShown >= kSprintFloor))
    {
        aWatch.FastSince = -1.0;
        aWatch.Started = false;
        return false;
    }
    if (aSprinting || aWatch.Started || !(aShown >= kSprintStartPace))
    {
        aWatch.FastSince = -1.0;
        return false;
    }
    if (aWatch.FastSince < 0.0)
    {
        aWatch.FastSince = aNow;
        return false;
    }
    if (aNow - aWatch.FastSince < kSprintSlowMs)
        return false;
    aWatch.FastSince = -1.0;
    aWatch.Started = true;
    return true;
}
} // namespace StrideMatch
