#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Math.hpp>
#include <Structs/VRPose.h>
#include <Systems/RemotePoseState.h>
#include <array>

// Shared receiver preparation. This knows the wire pose and playback tick, but no actor, skeleton layout or render hook.
namespace RemotePoseInterpolation
{
// Points must be ordered by Tick and expose VRPoseData. Missing poses are skipped; outside the pose range the nearest
// available pose is held, as before. Sparse finger/scale/weapon updates remain sticky in the presentation adapter.
template <class TPoints> [[nodiscard]] VRPose Sample(const TPoints& acPoints, const uint64_t aTick) noexcept
{
    const VRPose* pBefore = nullptr;
    const VRPose* pAfter = nullptr;
    uint64_t beforeTick = 0;
    uint64_t afterTick = 0;
    for (const auto& point : acPoints)
    {
        if (!point.VRPoseData.HasData)
            continue;
        if (point.Tick <= aTick)
        {
            pBefore = &point.VRPoseData;
            beforeTick = point.Tick;
        }
        else
        {
            pAfter = &point.VRPoseData;
            afterTick = point.Tick;
            break;
        }
    }
    if (!pBefore)
    {
        pBefore = pAfter;
        beforeTick = afterTick;
    }
    if (!pAfter)
    {
        pAfter = pBefore;
        afterTick = beforeTick;
    }

    // Always construct a fresh result: absent optional fields must not retain flags from an earlier frame/copy.
    VRPose pose{};
    if (!pBefore || !pAfter)
        return pose;

    float delta = 1.f;
    if (afterTick > beforeTick && aTick < afterTick)
        delta = aTick > beforeTick ? static_cast<float>(static_cast<double>(aTick - beforeTick) / static_cast<double>(afterTick - beforeTick)) : 0.f;

    pose.HasData = true;
    // A boneless endpoint has no rotations to blend. Legs are valid only when both endpoints carry them.
    pose.NoBones = pAfter->NoBones || pBefore->NoBones;
    if (!pose.NoBones)
    {
        pose.HasLegs = pBefore->HasLegs && pAfter->HasLegs;
        const size_t boneCount = pose.HasLegs ? VRPose::kBoneCount : VRPose::kUpperBoneCount;
        for (size_t i = 0; i < boneCount; ++i)
            pose.Bones[i] = glm::slerp(static_cast<glm::quat>(pBefore->Bones[i]), static_cast<glm::quat>(pAfter->Bones[i]), delta);
    }

    // Grip changes and scale are readings, not motion: use the newest set around the playback tick, without blending.
    const VRPose& fingerSource = pAfter->HasFingers ? *pAfter : *pBefore;
    if (fingerSource.HasFingers)
    {
        pose.HasFingers = true;
        pose.Fingers = fingerSource.Fingers;
    }
    const VRPose& scaleSource = pAfter->HasScale ? *pAfter : *pBefore;
    if (scaleSource.HasScale)
    {
        pose.HasScale = true;
        pose.RootScale = scaleSource.RootScale;
    }
    const VRPose& weaponSource = pAfter->HasWeapons ? *pAfter : *pBefore;
    if (weaponSource.HasWeapons)
    {
        pose.HasWeapons = true;
        pose.WeaponHeld = weaponSource.WeaponHeld;
        pose.WeaponRotation = weaponSource.WeaponRotation;
        for (size_t side = 0; side < 2; ++side)
            std::copy(std::begin(weaponSource.WeaponOffset[side]), std::end(weaponSource.WeaponOffset[side]), std::begin(pose.WeaponOffset[side]));
    }

    // Root and hips are transient offsets: blend when both ends have them; otherwise accept only the newer endpoint.
    pose.HasRootPosition = pAfter->HasRootPosition;
    pose.HasHips = pAfter->HasHips;
    for (size_t i = 0; i < 3; ++i)
    {
        if (pose.HasRootPosition)
            pose.RootPosition[i] = pBefore->HasRootPosition ? TiltedPhoques::Lerp(pBefore->RootPosition[i], pAfter->RootPosition[i], delta) : pAfter->RootPosition[i];
        if (pose.HasHips)
            pose.HipOffset[i] = pBefore->HasHips ? TiltedPhoques::Lerp(pBefore->HipOffset[i], pAfter->HipOffset[i], delta) : pAfter->HipOffset[i];
    }

    // Hand targets can be sparse on older senders. Hold the available set, blending only when both endpoints carry it.
    const VRPose& handSource = pAfter->HasHandCheck ? *pAfter : *pBefore;
    if (handSource.HasHandCheck)
    {
        pose.HasHandCheck = true;
        const bool blend = pBefore->HasHandCheck && pAfter->HasHandCheck;
        for (size_t i = 0; i < 3; ++i)
        {
            pose.LeftHandOffset[i] = blend ? TiltedPhoques::Lerp(pBefore->LeftHandOffset[i], pAfter->LeftHandOffset[i], delta) : handSource.LeftHandOffset[i];
            pose.RightHandOffset[i] = blend ? TiltedPhoques::Lerp(pBefore->RightHandOffset[i], pAfter->RightHandOffset[i], delta) : handSource.RightHandOffset[i];
        }
    }
    // Held spells sweep with the hand. Heading wraps at a full turn, while pitch is linear.
    const VRPose& spellSource = pAfter->HasSpellAim ? *pAfter : *pBefore;
    if (spellSource.HasSpellAim)
    {
        pose.HasSpellAim = true;
        if (pBefore->HasSpellAim && pAfter->HasSpellAim)
        {
            const float cTurn = std::remainder(pAfter->SpellAim[0] - pBefore->SpellAim[0], glm::two_pi<float>());
            pose.SpellAim[0] = pBefore->SpellAim[0] + cTurn * delta;
            pose.SpellAim[1] = TiltedPhoques::Lerp(pBefore->SpellAim[1], pAfter->SpellAim[1], delta);
        }
        else
            std::copy(std::begin(spellSource.SpellAim), std::end(spellSource.SpellAim), std::begin(pose.SpellAim));
    }
    return pose;
}

// Past the newest pose the body keeps moving the way it last moved, for at most this long.
inline constexpr double kMaxPoseExtrapolationMs = 100.0;
// Two poses further apart than this say nothing about how the body moves now.
inline constexpr double kMaxExtrapolationBaseMs = 250.0;
// Two poses closer than this are taken as this far apart.
inline constexpr double kMinExtrapolationBaseMs = 20.0;
// No bone turns further than this past the newest pose.
inline constexpr float kMaxExtrapolationDegrees = 30.f;
// No hand, root or hip offset is carried further than this many units past the newest pose.
inline constexpr float kMaxCarryUnits = 12.f;
// After a carried-on pose, the body is blended back to the poses that came over this long.
inline constexpr double kPoseBlendBackMs = 70.0;

// How far on, in ms of the last movement, an offset is carried aAheadMs past the newest pose: it slows down evenly and stops
// at aMaxMs, where it has gone half as far as at full speed.
[[nodiscard]] inline double CarriedMs(const double aAheadMs, const double aMaxMs) noexcept
{
    if (!(aAheadMs > 0.0) || !(aMaxMs > 0.0))
        return 0.0;
    const double cAhead = std::min(aAheadMs, aMaxMs);
    return cAhead - cAhead * cAhead / (2.0 * aMaxMs);
}

// How much of the last shown carried-on pose is still blended in aSinceMs after the late pose came: all of it at first,
// none from kPoseBlendBackMs on, easing in and out.
[[nodiscard]] inline float BlendBackWeight(const double aSinceMs) noexcept
{
    if (!(aSinceMs > 0.0))
        return 1.f;
    if (aSinceMs >= kPoseBlendBackMs)
        return 0.f;
    const double cShare = aSinceMs / kPoseBlendBackMs;
    return static_cast<float>(1.0 - cShare * cShare * (3.0 - 2.0 * cShare));
}

// The newer pose carried on past its tick, at the speed from the older pose to it: bones turn on, hand, root and hip
// offsets move on. Capped at aMaxMs past the newer tick and kMaxExtrapolationDegrees per bone. Every other reading is
// the newer pose's. With no usable older pose, or none of either, the newer pose is held as it is.
[[nodiscard]] inline VRPose Extrapolate(
    const VRPose& acOlder, const uint64_t aOlderTick, const VRPose& acNewer, const uint64_t aNewerTick, const uint64_t aTick,
    const double aMaxMs = kMaxPoseExtrapolationMs) noexcept
{
    VRPose pose = acNewer;
    if (!acOlder.HasData || !acNewer.HasData || aNewerTick <= aOlderTick || aTick <= aNewerTick || !(aMaxMs > 0.0))
        return pose;
    if (static_cast<double>(aNewerTick - aOlderTick) > kMaxExtrapolationBaseMs)
        return pose;
    // Poses only a few ms apart would carry a jitter far: the speed is taken over at least this long.
    const double cBase = std::max(static_cast<double>(aNewerTick - aOlderTick), kMinExtrapolationBaseMs);
    const double cAhead = std::min(static_cast<double>(aTick - aNewerTick), aMaxMs);
    const float cFactor = static_cast<float>(cAhead / cBase);

    if (!acOlder.NoBones && !acNewer.NoBones)
    {
        const size_t cBoneCount = acOlder.HasLegs && acNewer.HasLegs ? VRPose::kBoneCount : VRPose::kUpperBoneCount;
        const float cMaxAngle = glm::radians(kMaxExtrapolationDegrees);
        for (size_t i = 0; i < cBoneCount; ++i)
        {
            const glm::quat cFrom = acOlder.Bones[i];
            glm::quat to = acNewer.Bones[i];
            // The turn from the older rotation to the newer, on the short arc.
            glm::quat step = to * glm::inverse(cFrom);
            if (step.w < 0.f)
                step = -step;
            const glm::vec3 cImaginary(step.x, step.y, step.z);
            const float cLength = glm::length(cImaginary);
            if (!(cLength > 1e-7f))
                continue;
            const float cStepAngle = 2.f * std::atan2(cLength, step.w);
            const glm::vec3 cAxis = cImaginary / cLength;
            const float cAngle = std::min(cStepAngle * cFactor, cMaxAngle);
            pose.Bones[i] = glm::normalize(glm::angleAxis(cAngle, cAxis) * to);
        }
    }

    // Offsets slow down to a stop instead of shooting on at full speed, and go no further than kMaxCarryUnits.
    const float cOffsetFactor = static_cast<float>(CarriedMs(cAhead, aMaxMs) / cBase);
    const auto carry = [cOffsetFactor](const float(&acFrom)[3], const float(&acTo)[3], float(&aOut)[3]) noexcept
    {
        glm::vec3 step{};
        for (size_t i = 0; i < 3; ++i)
            step[i] = (acTo[i] - acFrom[i]) * cOffsetFactor;
        const float cLength = glm::length(step);
        if (!std::isfinite(cLength))
            step = {};
        else if (cLength > kMaxCarryUnits)
            step *= kMaxCarryUnits / cLength;
        for (size_t i = 0; i < 3; ++i)
            aOut[i] = acTo[i] + step[i];
    };
    if (acOlder.HasHandCheck && acNewer.HasHandCheck)
    {
        carry(acOlder.LeftHandOffset, acNewer.LeftHandOffset, pose.LeftHandOffset);
        carry(acOlder.RightHandOffset, acNewer.RightHandOffset, pose.RightHandOffset);
    }
    if (acOlder.HasRootPosition && acNewer.HasRootPosition)
        carry(acOlder.RootPosition, acNewer.RootPosition, pose.RootPosition);
    if (acOlder.HasHips && acNewer.HasHips)
        carry(acOlder.HipOffset, acNewer.HipOffset, pose.HipOffset);
    return pose;
}

// Unlike Sample(), this API requires receive provenance and an explicit stream generation. It does not mutate state,
// so holding the nearest endpoint across render frames cannot renew its age. TPoints expose Tick/VRPoseData/PoseSource.
template <class TPoints>
[[nodiscard]] RemotePose::Sample SampleWithProvenance(
    const TPoints& acPoints, const uint64_t aTick, const uint64_t aGeneration, const double aNowMs, const RemotePose::Policy aPolicy,
    const double aTimeoutMs = RemotePose::kLiveTimeoutMs, const double aMaxExtrapolationMs = 0.0) noexcept
{
    struct Endpoint
    {
        uint64_t Tick{};
        VRPose VRPoseData{};
        RemotePose::Provenance PoseSource{};
    };
    const typename TPoints::value_type* older = nullptr;
    const typename TPoints::value_type* before = nullptr;
    const typename TPoints::value_type* after = nullptr;
    for (const auto& point : acPoints)
    {
        if (!point.VRPoseData.HasData || point.PoseSource.SourceTick != point.Tick || !RemotePose::IsFresh(point.PoseSource, aGeneration, aNowMs, aPolicy, aTimeoutMs))
            continue;
        if (point.Tick <= aTick)
        {
            older = before;
            before = &point;
        }
        else
        {
            after = &point;
            break;
        }
    }
    RemotePose::Sample result{};
    result.Generation = aGeneration;
    // Played past the newest pose: carried on from the two newest, the pair already shown up to the newest.
    if (before && !after && older && aMaxExtrapolationMs > 0.0 && aTick > before->Tick)
    {
        result.Before = older->PoseSource;
        result.After = before->PoseSource;
        result.Pose = Sample(std::array<Endpoint, 1>{{{before->Tick, before->VRPoseData, before->PoseSource}}}, before->Tick);
        const VRPose cOlder = Sample(std::array<Endpoint, 1>{{{older->Tick, older->VRPoseData, older->PoseSource}}}, older->Tick);
        result.Pose = Extrapolate(cOlder, older->Tick, result.Pose, before->Tick, aTick, aMaxExtrapolationMs);
        return result;
    }
    if (!before)
        before = after;
    if (!after)
        after = before;
    if (!before || !after)
        return result;
    result.Before = before->PoseSource;
    result.After = after->PoseSource;
    // Bounded value copies; the returned snapshot has no references into a mutable network buffer.
    const std::array<Endpoint, 2> endpoints{{{before->Tick, before->VRPoseData, before->PoseSource}, {after->Tick, after->VRPoseData, after->PoseSource}}};
    result.Pose = Sample(endpoints, aTick);
    return result;
}
// The pose a share aToShare of the way from acFrom to acTo: bones turned, offsets moved, readings from acTo.
[[nodiscard]] inline VRPose Blend(const VRPose& acFrom, const VRPose& acTo, const float aToShare) noexcept
{
    struct Endpoint
    {
        uint64_t Tick{};
        VRPose VRPoseData{};
    };
    constexpr uint64_t cSteps = 1000;
    const float cShare = std::isfinite(aToShare) ? std::clamp(aToShare, 0.f, 1.f) : 1.f;
    const std::array<Endpoint, 2> cEndpoints{{{0, acFrom}, {cSteps, acTo}}};
    return Sample(cEndpoints, static_cast<uint64_t>(std::lround(cShare * static_cast<float>(cSteps))));
}

// Whether acSample carries the body on past its newest pose: drawn from the two newest poses, ahead of the newer one.
[[nodiscard]] inline bool IsCarried(const RemotePose::Sample& acSample, const uint64_t aTick) noexcept
{
    return acSample.Before.Valid && acSample.After.Valid && acSample.Before.SourceTick < acSample.After.SourceTick && aTick > acSample.After.SourceTick;
}

// Whether two samples were drawn from the same pair of poses.
[[nodiscard]] inline bool SamePair(const RemotePose::Sample& acLeft, const RemotePose::Sample& acRight) noexcept
{
    return acLeft.Before.Valid == acRight.Before.Valid && acLeft.After.Valid == acRight.After.Valid && acLeft.Before.SourceTick == acRight.Before.SourceTick &&
           acLeft.After.SourceTick == acRight.After.SourceTick;
}

// What a VR body's receiver keeps between frames to blend it back after it was carried on.
struct BlendBack
{
    // The pose last shown was carried on past the newest pose.
    bool Carried{false};
    VRPose From{};
    // When the blend began, or below zero when there is none.
    double Since{-1.0};
    uint64_t Generation{};
};

// Turns aSample, taken at aTick, into the pose to show after acShown. Once a carried-on body is drawn from another pair
// of poses (a late pose came, or a second one), it is blended there from the pose last shown, so the part of the carry
// still on screen fades out over kPoseBlendBackMs instead of snapping. Running past the newest pose again during the
// blend does not end it: the carry goes on from the same pair. A new stream generation or an empty pose ends it.
inline void Present(BlendBack& aState, const RemotePose::Sample& acShown, RemotePose::Sample& aSample, const uint64_t aTick, const double aNowMs) noexcept
{
    const bool cCarried = IsCarried(aSample, aTick);
    if (aState.Carried && !SamePair(aSample, acShown) && acShown.Pose.HasData)
    {
        aState.From = acShown.Pose;
        aState.Since = aNowMs;
        aState.Generation = acShown.Generation;
    }
    aState.Carried = cCarried;
    if (aState.Since < 0.0)
        return;
    const float cWeight = BlendBackWeight(aNowMs - aState.Since);
    if (cWeight <= 0.f || !aSample.Pose.HasData || aSample.Generation != aState.Generation)
        aState.Since = -1.0;
    else
        aSample.Pose = Blend(aState.From, aSample.Pose, 1.f - cWeight);
}
} // namespace RemotePoseInterpolation
