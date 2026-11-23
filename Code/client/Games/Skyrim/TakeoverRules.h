#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Values only: what an NPC this game has just taken over does with its legs, read from its body a few times a second.
namespace TakeoverRules
{
// How long after a gain an NPC's legs are watched, how often they are read, and how long a reading is judged over.
inline constexpr int64_t kWatchMs = 6000;
inline constexpr int64_t kFirstLookMs = 300;
inline constexpr int64_t kSampleEveryMs = 125;
inline constexpr int64_t kWindowMs = 750;
// Travel over a window that a standing body never makes (about 27 units a second, a slow walk is 60 or more).
inline constexpr float kMinTravel = 20.f;
// How far each foot's horizontal distance from the pelvis changes over a window while it steps: a stride is 15 units or more.
inline constexpr float kMinStride = 4.f;
// The engine's own start is tried, then the graph is put back to its default state and started again; past that it is left alone.
inline constexpr uint32_t kMaxMends = 2;

struct Sample
{
    int64_t AtMs{};
    float X{};
    float Y{};
    float LeftFoot{};
    float RightFoot{};
};

enum class Legs
{
    kUnknown, // too few readings over the window
    kStand,   // the body does not travel
    kWalk,    // it travels, and a foot steps
    kSlide,   // it travels on still legs
};

inline bool Finite(const Sample& acSample) noexcept
{
    return std::isfinite(acSample.X) && std::isfinite(acSample.Y) && std::isfinite(acSample.LeftFoot) && std::isfinite(acSample.RightFoot);
}

// The readings that fall in the last window, oldest first, judged as the rig judges a slide.
inline Legs Judge(const std::vector<Sample>& acSamples, int64_t aNowMs) noexcept
{
    const Sample* pFirst = nullptr;
    const Sample* pLast = nullptr;
    float travel = 0.f;
    float leftMin = 0.f, leftMax = 0.f, rightMin = 0.f, rightMax = 0.f;
    size_t count = 0;
    for (const auto& cSample : acSamples)
    {
        if (aNowMs - cSample.AtMs > kWindowMs || cSample.AtMs > aNowMs || !Finite(cSample))
            continue;
        if (pLast)
            travel += std::hypot(cSample.X - pLast->X, cSample.Y - pLast->Y);
        else
        {
            pFirst = &cSample;
            leftMin = leftMax = cSample.LeftFoot;
            rightMin = rightMax = cSample.RightFoot;
        }
        leftMin = std::min(leftMin, cSample.LeftFoot);
        leftMax = std::max(leftMax, cSample.LeftFoot);
        rightMin = std::min(rightMin, cSample.RightFoot);
        rightMax = std::max(rightMax, cSample.RightFoot);
        pLast = &cSample;
        ++count;
    }
    // At least four readings spread over most of the window.
    if (count < 4 || pLast->AtMs - pFirst->AtMs < kWindowMs * 2 / 3)
        return Legs::kUnknown;
    if (travel < kMinTravel)
        return Legs::kStand;
    return std::max(leftMax - leftMin, rightMax - rightMin) >= kMinStride ? Legs::kWalk : Legs::kSlide;
}

enum class Mend
{
    kNone,       // nothing to do
    kStart,      // perform the engine's ActionMoveStart, as its AI would have
    kResetStart, // put the graph back to its default state ("IdleForceDefaultState"), then perform ActionMoveStart
    kGiveUp,     // two tries did not get its legs going: left alone
};

// What a gained NPC whose legs were just judged needs. Each mend waits a full window of new readings before the next.
// Only a body this game's AI moves (it gives it a speed) is started: one pushed or carried along stands as it should.
inline Mend NextMend(Legs aLegs, bool aAiMoves, uint32_t aMendsDone, bool aWindowSinceLastMend) noexcept
{
    if (aLegs != Legs::kSlide || !aAiMoves || !aWindowSinceLastMend)
        return Mend::kNone;
    if (aMendsDone == 0)
        return Mend::kStart;
    if (aMendsDone == 1)
        return Mend::kResetStart;
    return aMendsDone == kMaxMends ? Mend::kGiveUp : Mend::kNone;
}

// Only an NPC this game runs, on its feet, alive and up, is mended: anything else moves its body on purpose (a ragdoll, a mount, furniture).
inline bool CanMend(bool aOurs, bool aOnFoot, bool aDeadOrDying, bool aBleedingOut, bool aKnockedDown, bool aMount) noexcept
{
    return aOurs && aOnFoot && !aDeadOrDying && !aBleedingOut && !aKnockedDown && !aMount;
}
// A new copy (another game's NPC shown here for the first time) is moved by its owner's reports. It walks if they cover more than this many units a second.
inline constexpr float kOwnerMovingSpeed = 20.f;

inline bool OwnerMoves(float aReportedSpeed) noexcept
{
    return std::isfinite(aReportedSpeed) && aReportedSpeed > kOwnerMovingSpeed;
}

// A new NPC copy is started on its own graph only when its owner's own graph drives it: not another creature kind's graph, which keeps its own state.
inline bool CanMendCopy(bool aNpcCopy, bool aForeignGraph, bool aOnFoot, bool aDeadOrDying, bool aBleedingOut, bool aKnockedDown, bool aMount) noexcept
{
    return aNpcCopy && !aForeignGraph && CanMend(true, aOnFoot, aDeadOrDying, aBleedingOut, aKnockedDown, aMount);
}
} // namespace TakeoverRules
