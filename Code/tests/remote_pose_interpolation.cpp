#include <catch2/catch.hpp>

#include <Systems/RemotePoseInterpolation.h>

#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
struct Point
{
    uint64_t Tick;
    VRPose VRPoseData;
};

enum Optional : unsigned
{
    Fingers = 1,
    Scale = 2,
    Root = 4,
    Hips = 8,
    Hands = 16,
    Weapons = 32,
    All = 63
};

VRPose MakePose(const float aValue, const unsigned aFields = All)
{
    VRPose pose{};
    pose.HasData = true;
    pose.HasLegs = true;
    for (auto& bone : pose.Bones)
        bone = glm::quat(1.f, 0.f, 0.f, 0.f);
    pose.HasFingers = (aFields & Fingers) != 0;
    for (auto& finger : pose.Fingers)
        finger = glm::angleAxis(glm::radians(aValue), glm::vec3(0.f, 0.f, 1.f));
    pose.HasScale = (aFields & Scale) != 0;
    pose.RootScale = static_cast<uint16_t>(1000.f + aValue);
    pose.HasRootPosition = (aFields & Root) != 0;
    pose.HasHips = (aFields & Hips) != 0;
    pose.HasHandCheck = (aFields & Hands) != 0;
    pose.HasWeapons = (aFields & Weapons) != 0;
    pose.WeaponHeld = {aValue < 20.f, aValue >= 20.f};
    for (size_t side = 0; side < 2; ++side)
    {
        pose.WeaponRotation[side] = glm::angleAxis(glm::radians(aValue + static_cast<float>(side)), glm::vec3(0.f, 1.f, 0.f));
        for (size_t i = 0; i < 3; ++i)
            pose.WeaponOffset[side][i] = aValue + static_cast<float>(side * 10 + i);
    }
    for (size_t i = 0; i < 3; ++i)
    {
        pose.RootPosition[i] = aValue + static_cast<float>(i);
        pose.HipOffset[i] = aValue + 10.f + static_cast<float>(i);
        pose.LeftHandOffset[i] = aValue + 20.f + static_cast<float>(i);
        pose.RightHandOffset[i] = aValue + 30.f + static_cast<float>(i);
    }
    return pose;
}

void RequireEmpty(const VRPose& acPose)
{
    REQUIRE_FALSE(acPose.HasData);
    REQUIRE_FALSE(acPose.NoBones);
    REQUIRE_FALSE(acPose.HasLegs);
    REQUIRE_FALSE(acPose.HasFingers);
    REQUIRE_FALSE(acPose.HasScale);
    REQUIRE_FALSE(acPose.HasRootPosition);
    REQUIRE_FALSE(acPose.HasHips);
    REQUIRE_FALSE(acPose.HasHandCheck);
    REQUIRE_FALSE(acPose.HasWeapons);
}

void RequireRotation(const glm::quat& acActual, const glm::quat& acExpected)
{
    REQUIRE(glm::length(acActual) == Approx(1.f).margin(0.00001f));
    // q and -q describe the same rotation.
    REQUIRE(std::abs(glm::dot(acActual, acExpected)) == Approx(1.f).margin(0.00001f));
}
} // namespace

TEST_CASE("Remote pose receiver selects and holds poses around the movement playback tick", "[remote_pose]")
{
    const auto first = MakePose(10.f);
    const auto last = MakePose(30.f);
    // A movement report without a pose must not be treated as a bone rotation or an origin.
    auto missing = MakePose(1000.f);
    missing.HasData = false;
    const std::vector<Point> points{{100, first}, {150, missing}, {200, last}};
    REQUIRE(RemotePoseInterpolation::Sample(points, 0).RootPosition[0] == 10.f);
    REQUIRE(RemotePoseInterpolation::Sample(points, 100).RootPosition[0] == 10.f);
    REQUIRE(RemotePoseInterpolation::Sample(points, 125).RootPosition[0] == 15.f);
    REQUIRE(RemotePoseInterpolation::Sample(points, 150).RootPosition[0] == 20.f);
    REQUIRE(RemotePoseInterpolation::Sample(points, 200).RootPosition[0] == 30.f);
    REQUIRE(RemotePoseInterpolation::Sample(points, 10000).RootPosition[0] == 30.f);

    RequireEmpty(RemotePoseInterpolation::Sample(std::vector<Point>{}, 150));
    RequireEmpty(RemotePoseInterpolation::Sample(std::vector<Point>{{100, missing}, {200, missing}}, 150));
    REQUIRE(RemotePoseInterpolation::Sample(std::vector<Point>{{100, first}}, 10000).RootPosition[0] == 10.f);
}

TEST_CASE("Remote pose receiver preserves every optional field policy", "[remote_pose]")
{
    // Every pair of optional-field combinations, including a field arriving or disappearing on either side.
    for (unsigned beforeMask = 0; beforeMask <= All; ++beforeMask)
    {
        for (unsigned afterMask = 0; afterMask <= All; ++afterMask)
        {
            INFO("before fields " << beforeMask << ", after fields " << afterMask);
            const auto before = MakePose(10.f, beforeMask);
            const auto after = MakePose(30.f, afterMask);
            const auto pose = RemotePoseInterpolation::Sample(std::array<Point, 2>{{{100, before}, {200, after}}}, 150);
            REQUIRE(pose.HasData);
            REQUIRE(pose.HasLegs);
            REQUIRE_FALSE(pose.NoBones);
            REQUIRE(pose.HasFingers == (((beforeMask | afterMask) & Fingers) != 0));
            REQUIRE(pose.HasScale == (((beforeMask | afterMask) & Scale) != 0));
            REQUIRE(pose.HasWeapons == (((beforeMask | afterMask) & Weapons) != 0));
            REQUIRE(pose.HasHandCheck == (((beforeMask | afterMask) & Hands) != 0));
            REQUIRE(pose.HasRootPosition == after.HasRootPosition);
            REQUIRE(pose.HasHips == after.HasHips);
            if (pose.HasFingers)
                REQUIRE(pose.Fingers == (after.HasFingers ? after.Fingers : before.Fingers));
            if (pose.HasScale)
                REQUIRE(pose.RootScale == (after.HasScale ? 1030 : 1010));
            if (pose.HasWeapons)
            {
                const auto& source = after.HasWeapons ? after : before;
                REQUIRE(pose.WeaponHeld == source.WeaponHeld);
                REQUIRE(pose.WeaponRotation == source.WeaponRotation);
                for (size_t side = 0; side < 2; ++side)
                    for (size_t i = 0; i < 3; ++i)
                        REQUIRE(pose.WeaponOffset[side][i] == source.WeaponOffset[side][i]);
            }
            for (size_t i = 0; i < 3; ++i)
            {
                const float index = static_cast<float>(i);
                if (pose.HasRootPosition)
                    REQUIRE(pose.RootPosition[i] == (before.HasRootPosition ? 20.f : 30.f) + index);
                if (pose.HasHips)
                    REQUIRE(pose.HipOffset[i] == (before.HasHips ? 30.f : 40.f) + index);
                if (pose.HasHandCheck)
                {
                    const float handValue = before.HasHandCheck ? (after.HasHandCheck ? 20.f : 10.f) : 30.f;
                    REQUIRE(pose.LeftHandOffset[i] == handValue + 20.f + index);
                    REQUIRE(pose.RightHandOffset[i] == handValue + 30.f + index);
                }
            }
        }
    }
}

TEST_CASE("Remote pose receiver blends bone rotations on the short arc and never extrapolates", "[remote_pose]")
{
    auto before = MakePose(10.f);
    auto after = MakePose(30.f);
    const glm::vec3 axis(0.f, 0.f, 1.f);
    const glm::quat start = glm::angleAxis(glm::radians(170.f), axis);
    const glm::quat end = glm::angleAxis(glm::radians(-170.f), axis);
    for (auto& bone : before.Bones)
        bone = start;
    for (auto& bone : after.Bones)
        bone = end;
    const std::array<Point, 2> points{{{100, before}, {200, after}}};
    const auto middle = RemotePoseInterpolation::Sample(points, 150);
    for (size_t i = 0; i < VRPose::kBoneCount; ++i)
        RequireRotation(middle.Bones[i], glm::angleAxis(glm::radians(180.f), axis));
    RequireRotation(RemotePoseInterpolation::Sample(points, 0).Bones[0], start);
    RequireRotation(RemotePoseInterpolation::Sample(points, 10000).Bones[0], end);

    // Opposite quaternion signs must hold the same orientation rather than collapsing to a zero quaternion.
    for (auto& bone : after.Bones)
        bone = -start;
    const auto antipodal = RemotePoseInterpolation::Sample(std::array<Point, 2>{{{100, before}, {200, after}}}, 150);
    RequireRotation(antipodal.Bones[0], start);
}

TEST_CASE("Remote pose receiver requires valid bones at both endpoints", "[remote_pose]")
{
    for (unsigned mask = 0; mask < 16; ++mask)
    {
        auto before = MakePose(10.f);
        auto after = MakePose(30.f);
        before.HasLegs = (mask & 1) != 0;
        after.HasLegs = (mask & 2) != 0;
        before.NoBones = (mask & 4) != 0;
        after.NoBones = (mask & 8) != 0;
        const auto pose = RemotePoseInterpolation::Sample(std::array<Point, 2>{{{100, before}, {200, after}}}, 150);
        REQUIRE(pose.NoBones == (before.NoBones || after.NoBones));
        REQUIRE(pose.HasLegs == (!pose.NoBones && before.HasLegs && after.HasLegs));
        // Boneless corpse/root placement and the other readings still pass through.
        REQUIRE(pose.HasRootPosition);
        REQUIRE(pose.RootPosition[0] == 20.f);
        REQUIRE(pose.HasWeapons);
    }
}

TEST_CASE("Remote pose receiver clears transient fields between frames and new buffers", "[remote_pose]")
{
    auto pose = RemotePoseInterpolation::Sample(std::vector<Point>{{100, MakePose(10.f)}}, 100);
    REQUIRE(pose.HasHips);
    REQUIRE(pose.HasHandCheck);
    pose = RemotePoseInterpolation::Sample(std::vector<Point>{{200, MakePose(30.f, 0)}}, 200);
    REQUIRE(pose.HasData);
    REQUIRE_FALSE(pose.HasHips);
    REQUIRE_FALSE(pose.HasHandCheck);
    REQUIRE_FALSE(pose.HasRootPosition);
    REQUIRE_FALSE(pose.HasFingers);
    REQUIRE_FALSE(pose.HasScale);
    REQUIRE_FALSE(pose.HasWeapons);
    pose = RemotePoseInterpolation::Sample(std::vector<Point>{{300, VRPose{}}}, 300);
    RequireEmpty(pose);
    pose = RemotePoseInterpolation::Sample(std::vector<Point>{{400, MakePose(50.f)}}, 400);
    REQUIRE(pose.RootPosition[0] == 50.f);
    REQUIRE(pose.RootScale == 1050);
}

TEST_CASE("Remote pose receiver handles duplicate ticks and large owner clocks without subtraction wrap", "[remote_pose]")
{
    const auto first = MakePose(10.f);
    const auto last = MakePose(30.f);
    REQUIRE(RemotePoseInterpolation::Sample(std::array<Point, 2>{{{100, first}, {100, last}}}, 100).RootPosition[0] == 30.f);
    const uint64_t end = std::numeric_limits<uint64_t>::max();
    const std::array<Point, 2> points{{{end - 100, first}, {end, last}}};
    REQUIRE(RemotePoseInterpolation::Sample(points, end - 50).RootPosition[0] == 20.f);
    REQUIRE(RemotePoseInterpolation::Sample(points, 0).RootPosition[0] == 10.f);
    REQUIRE(RemotePoseInterpolation::Sample(points, end).RootPosition[0] == 30.f);
}
