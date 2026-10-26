#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>

#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Math.hpp>
#include <Structs/VRPose.h>

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
    return pose;
}
} // namespace RemotePoseInterpolation
