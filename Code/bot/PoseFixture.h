#pragma once

#include <glm/gtc/quaternion.hpp>
#include <Structs/VRPose.h>
#include <cstdint>
#include <optional>
#include <string_view>

// Synthetic protocol fixtures only: these are actor-root-relative rotations/offsets,
// not a claim about any game's node layout or calibrated controller coordinates.
namespace PoseFixture
{
enum class Phase { A, B, NoData, Recovery };

inline std::optional<Phase> Parse(std::string_view aName) noexcept
{
    if (aName == "a") return Phase::A;
    if (aName == "b") return Phase::B;
    if (aName == "none") return Phase::NoData;
    if (aName == "recovery") return Phase::Recovery;
    return std::nullopt;
}

inline VRPose Make(Phase aPhase) noexcept
{
    VRPose pose{};
    if (aPhase == Phase::NoData)
        return pose;

    const float seed = aPhase == Phase::A ? 1.f : aPhase == Phase::B ? -2.f : 3.f;
    pose.HasData = true;
    pose.HasLegs = true;
    for (size_t i = 0; i < VRPose::kBoneCount; ++i)
        pose.Bones[i] = glm::normalize(glm::quat(1.f, seed * (i + 1) * .013f, seed * .031f, (i + 1) * .009f));
    pose.HasFingers = true;
    for (size_t i = 0; i < VRPose::kFingerBoneCount; ++i)
        pose.Fingers[i] = glm::normalize(glm::quat(1.f, seed * .017f, (i + 1) * .011f, seed * .023f));
    pose.HasScale = true;
    pose.RootScale = aPhase == Phase::A ? 875 : aPhase == Phase::B ? 1125 : 1375;
    pose.HasHips = true;
    pose.HasHandCheck = true;
    pose.HasWeapons = true;
    pose.WeaponHeld = {true, true};
    for (size_t side = 0; side < 2; ++side)
    {
        pose.WeaponRotation[side] = glm::normalize(glm::quat(1.f, seed * .07f, (side + 1) * .11f, seed * .09f));
        for (size_t axis = 0; axis < 3; ++axis)
            pose.WeaponOffset[side][axis] = seed * (side + 1) * (axis + 1) * 2.25f;
    }
    for (size_t axis = 0; axis < 3; ++axis)
    {
        pose.HipOffset[axis] = seed * (axis + 1) * 4.25f;
        pose.LeftHandOffset[axis] = seed * (axis + 1) * -12.5f;
        pose.RightHandOffset[axis] = seed * (axis + 1) * 15.75f;
    }
    return pose;
}

// Filled only by the bot's server-message receive handler. Count prevents a default
// or an old snapshot from satisfying a phase; SawData makes none a real transition.
struct Observation
{
    VRPose Last{};
    uint64_t Count{};
    bool SawData{};

    void Receive(const VRPose& acPose) noexcept
    {
        Last = acPose;
        ++Count;
        SawData = SawData || acPose.HasData;
    }

    bool Matches(Phase aPhase, uint64_t aAfter) const noexcept
    {
        return Count > aAfter && (aPhase != Phase::NoData || SawData) && Last == Make(aPhase);
    }
};
}
