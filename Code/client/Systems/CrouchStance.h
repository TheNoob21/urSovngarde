#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <Structs/AnimationVariables.h>
#include <NpcFurniture.h>

//! A VR player crouching for real, without the sneak toggle and without leg trackers, is shown on their copy in the sneak
//! stance. Their pose's hips drop, but without tracked legs the copy's legs are its own walk animation, so lowering the body to
//! that height would sink its feet: the copy's graph plays its sneak stance instead, on the receiving game only.
//! The hip height is the pelvis above the 3D root, in game units, as the owner's pose sends it (VRPose::HipOffset).
namespace CrouchStance
{
//! Crouched once the hips are below this share of the standing height, standing again once above kLeaveShare of it.
inline constexpr float kEnterShare = 0.75f;
inline constexpr float kLeaveShare = 0.85f;
//! A reading has to stay past the line this long before the stance changes, so a bob or one bad pose does not flicker it.
inline constexpr double kHoldMs = 250.0;
//! A standing height outside this range is no body's: nothing is shown until a believable one is known.
inline constexpr float kMinStanding = 30.f;
inline constexpr float kMaxStanding = 150.f;
//! The standing height rises to a higher reading at this many units a second, so one spike does not make everything a crouch.
inline constexpr float kRisePerSecond = 120.f;
//! And settles to a lower standing reading at this many, so a slouch or a smaller body is followed but a crouch is not.
inline constexpr float kSettlePerSecond = 1.f;

//! One copy's standing height, its stance, and since when a reading has been past the line.
struct Watch
{
    float Standing{-1.f};
    bool Crouched{false};
    double Since{-1.0};
    double LastAt{-1.0};
};

//! Whether the copy is shown crouched after the hip height aHip read at aNow (ms). A reading that is not a number counts as
//! standing. aAllowed is false while the crouch is shown some other way or not at all: the owner's legs are sent, their sneak
//! toggle is on, or the copy is not on its feet. Then it stands at once, and its standing height is kept.
inline bool Update(Watch& aWatch, const bool aAllowed, const float aHip, const double aNow) noexcept
{
    const double cElapsed = aWatch.LastAt >= 0.0 ? std::clamp(aNow - aWatch.LastAt, 0.0, 1000.0) : 0.0;
    aWatch.LastAt = aNow;
    if (!aAllowed)
    {
        aWatch.Crouched = false;
        aWatch.Since = -1.0;
        return false;
    }

    const bool cReading = std::isfinite(aHip) && aHip > 0.f;
    if (cReading)
    {
        const float cSeconds = static_cast<float>(cElapsed / 1000.0);
        if (aWatch.Standing < 0.f)
            aWatch.Standing = std::min(aHip, kMaxStanding);
        else if (aHip > aWatch.Standing)
            aWatch.Standing = std::min({aHip, aWatch.Standing + kRisePerSecond * cSeconds, kMaxStanding});
        else if (!aWatch.Crouched && aHip >= aWatch.Standing * kLeaveShare)
            aWatch.Standing = std::max(aHip, aWatch.Standing - kSettlePerSecond * cSeconds);
    }

    bool wanted = false;
    if (cReading && aWatch.Standing >= kMinStanding)
        wanted = aHip < aWatch.Standing * (aWatch.Crouched ? kLeaveShare : kEnterShare);

    if (wanted == aWatch.Crouched)
        aWatch.Since = -1.0;
    else if (aWatch.Since < 0.0)
        aWatch.Since = aNow;
    else if (aNow - aWatch.Since >= kHoldMs)
    {
        aWatch.Crouched = wanted;
        aWatch.Since = -1.0;
    }
    return aWatch.Crouched;
}

//! Swimming, bit 10 of the actor state's first flags.
inline constexpr uint32_t kSwimmingBit = 1u << 10;

//! Whether the copy's hips are low for something other than a crouch: seated or sleeping in furniture, or swimming (its actor
//! state's first flags aFlags1), or riding (aMounted). Its graph plays that, not the sneak stance.
inline bool LowForOtherReason(const uint32_t aFlags1, const bool aMounted) noexcept
{
    return aMounted || (aFlags1 & (NpcFurniture::kSitSleepMask | kSwimmingBit)) != 0;
}

//! Whether the owner's own variables have them sneaking: its "IsSneaking" switch at aSneakingSlot (-1 or past the list: no).
inline bool OwnerSneaks(const AnimationVariables& acVariables, const int aSneakingSlot) noexcept
{
    return aSneakingSlot >= 0 && static_cast<size_t>(aSneakingSlot) < acVariables.Booleans.size() && acVariables.Booleans[aSneakingSlot];
}

//! Writes the sneak stance's variables over the owner's, so loading them does not take the copy's graph out of it:
//! "IsSneaking" at aSneakingSlot and "iIsInSneak" at aInSneakSlot. A slot that is negative or past its list is left alone.
inline void Hold(AnimationVariables& aVariables, const int aSneakingSlot, const int aInSneakSlot) noexcept
{
    if (aSneakingSlot >= 0 && static_cast<size_t>(aSneakingSlot) < aVariables.Booleans.size())
        aVariables.Booleans[aSneakingSlot] = true;
    if (aInSneakSlot >= 0 && static_cast<size_t>(aInSneakSlot) < aVariables.Integers.size())
        aVariables.Integers[aInSneakSlot] = 1;
}
} // namespace CrouchStance
