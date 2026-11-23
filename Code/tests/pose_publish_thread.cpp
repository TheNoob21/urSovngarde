#include <catch2/catch.hpp>

#include <Systems/PosePublishThread.h>
#include <Systems/RemotePoseInterpolation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace
{
// The game's job threads as the client's update met them on the PCVR rig (2026-11-18): a different one most frames.
constexpr std::array<uint32_t, 6> cJobThreads{24532, 56568, 23528, 39716, 18552, 10360};

uint32_t UpdateThread(const size_t aFrame) noexcept
{
    // Not strictly in turn: now and then the same worker takes two updates running, as the game takes whichever is free.
    return cJobThreads[(aFrame * 5 + aFrame / 6) % cJobThreads.size()];
}

// How many of aFrames client updates publish a pose: Tick() at the start of the update (the owner is its thread when aFollowUpdate),
// then Publish() on the same thread. Without it the first thread to publish keeps it for good, as it was.
size_t PublishedFrames(const size_t aFrames, const bool aFollowUpdate)
{
    uint32_t owner = 0;
    size_t published = 0;
    for (size_t frame = 0; frame < aFrames; ++frame)
    {
        const uint32_t cThread = UpdateThread(frame);
        if (aFollowUpdate)
            owner = cThread;
        owner = PosePublishThread::OwnerForPublish(owner, cThread);
        if (PosePublishThread::MayPublish(owner, cThread))
            ++published;
    }
    return published;
}

struct Point
{
    uint64_t Tick;
    VRPose VRPoseData;
};

// A hand moving one unit a ms, as the headset game sends it.
VRPose HandAt(const uint64_t aTick)
{
    VRPose pose{};
    pose.HasData = true;
    pose.HasLegs = true;
    for (auto& bone : pose.Bones)
        bone = glm::angleAxis(glm::radians(static_cast<float>(aTick) * 0.5f), glm::vec3(0.f, 0.f, 1.f));
    pose.HasHandCheck = true;
    for (size_t i = 0; i < 3; ++i)
    {
        pose.LeftHandOffset[i] = static_cast<float>(aTick);
        pose.RightHandOffset[i] = static_cast<float>(aTick);
    }
    return pose;
}
} // namespace

TEST_CASE("A remote body's pose is published on every client update, on whichever job thread it runs")
{
    // A second at 60 fps.
    REQUIRE(PublishedFrames(60, true) == 60);
    // As it was: only the updates that came on the first thread published, about one in six.
    REQUIRE(PublishedFrames(60, false) == 10);
}

TEST_CASE("Pose publication ownership")
{
    // None yet: the first to publish takes it.
    REQUIRE(PosePublishThread::OwnerForPublish(0, 7) == 7);
    REQUIRE(PosePublishThread::OwnerForPublish(5, 7) == 5);
    REQUIRE_FALSE(PosePublishThread::MayPublish(0, 0));
    REQUIRE_FALSE(PosePublishThread::MayPublish(5, 7));
    REQUIRE(PosePublishThread::MayPublish(7, 7));
}

TEST_CASE("A body published every frame moves every frame between poses sent 33 ms apart")
{
    // Poses as the headset game sends them, 30 a second, over a second.
    std::vector<Point> points;
    for (uint64_t tick = 1000; tick <= 2100; tick += 33)
        points.push_back({tick, HandAt(tick)});

    // The flat game at 60 fps, shown 100 ms behind: what the body commit writes on each frame.
    const auto shownHands = [&](const bool aFollowUpdate)
    {
        std::vector<float> hands;
        uint32_t owner = 0;
        float shown = 0.f;
        for (size_t frame = 0; frame < 60; ++frame)
        {
            const uint64_t cTick = 1100 + static_cast<uint64_t>(std::lround(static_cast<double>(frame) * 1000.0 / 60.0));
            const uint32_t cThread = UpdateThread(frame);
            if (aFollowUpdate)
                owner = cThread;
            owner = PosePublishThread::OwnerForPublish(owner, cThread);
            if (PosePublishThread::MayPublish(owner, cThread))
                shown = RemotePoseInterpolation::Sample(points, cTick).RightHandOffset[0];
            hands.push_back(shown);
        }
        return hands;
    };
    const auto countMoves = [](const std::vector<float>& acHands)
    {
        size_t moves = 0;
        float largest = 0.f;
        for (size_t i = 1; i < acHands.size(); ++i)
        {
            const float cStep = std::fabs(acHands[i] - acHands[i - 1]);
            if (cStep > 0.01f)
                ++moves;
            largest = std::max(largest, cStep);
        }
        return std::pair<size_t, float>{moves, largest};
    };

    // Every frame: 59 even steps of a frame's worth of movement (about 17 units).
    const auto [cMoves, cLargest] = countMoves(shownHands(true));
    REQUIRE(cMoves == 59);
    REQUIRE(cLargest < 18.f);

    // As it was: held most frames, then a jump of several frames' worth.
    const auto [cLatchedMoves, cLatchedLargest] = countMoves(shownHands(false));
    REQUIRE(cLatchedMoves <= 10);
    REQUIRE(cLatchedLargest > 50.f);
}
