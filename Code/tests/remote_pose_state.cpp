#include <catch2/catch.hpp>
#include <Systems/RemotePoseInterpolation.h>

#include <array>
#include <limits>
#include <vector>

namespace
{
struct PosePoint
{
    uint64_t Tick{};
    VRPose VRPoseData{};
    RemotePose::Provenance PoseSource{};
};

VRPose TrackedPose()
{
    VRPose pose{};
    pose.HasData = true;
    pose.HasLegs = true;
    for (auto& bone : pose.Bones)
        bone = glm::quat(1.f, 0.f, 0.f, 0.f);
    return pose;
}

PosePoint Point(RemotePose::State& aState, uint64_t aTick, double aReceived, const VRPose& acPose, RemotePose::Policy aPolicy = RemotePose::Policy::LiveVRPlayer)
{
    const auto accepted = aState.Accept(aTick, aReceived, acPose, aPolicy);
    REQUIRE(accepted.AcceptedPoint);
    return {aTick, accepted.Pose, accepted.Source};
}

RemotePose::Sample
Sample(const std::vector<PosePoint>& acPoints, RemotePose::State& aState, uint64_t aTick, double aNow, RemotePose::Policy aPolicy = RemotePose::Policy::LiveVRPlayer)
{
    return RemotePoseInterpolation::SampleWithProvenance(acPoints, aTick, aState.Generation(), aNow, aPolicy);
}
} // namespace

TEST_CASE("Remote pose samples retain actual endpoint receive provenance", "[remote_pose][remote_pose_state]")
{
    RemotePose::State state;
    auto first = TrackedPose();
    first.HasRootPosition = true;
    first.RootPosition[0] = 10.f;
    auto last = first;
    last.RootPosition[0] = 30.f;
    const std::vector<PosePoint> points{Point(state, 100, 1000., first), Point(state, 200, 1100., last)};
    const auto middle = Sample(points, state, 150, 1200.);
    REQUIRE(middle.Pose.RootPosition[0] == Approx(20.f));
    REQUIRE(middle.Before.SourceTick == 100);
    REQUIRE(middle.Before.ReceivedAtMs == 1000.);
    REQUIRE(middle.After.SourceTick == 200);
    REQUIRE(middle.After.ReceivedAtMs == 1100.);
    REQUIRE(middle.Generation == state.Generation());
    REQUIRE(Sample(points, state, 999999, 2100.).Pose.HasData);
    REQUIRE_FALSE(Sample(points, state, 999999, 2100.001).Pose.HasData);
    // Neither advancing the playback tick nor repeatedly sampling updates receive timestamps.
    REQUIRE(points.back().PoseSource.ReceivedAtMs == 1100.);
    REQUIRE_FALSE(Sample(points, state, 9999999, 5000.).Pose.HasData);
}

TEST_CASE("Missing pose reports do not renew tracking or discard settled bodies", "[remote_pose][remote_pose_state]")
{
    RemotePose::State live;
    std::vector<PosePoint> points{Point(live, 1, 1000., TrackedPose())};
    points.push_back(Point(live, 2, 1800., VRPose{}));
    REQUIRE_FALSE(points.back().PoseSource.Valid);
    REQUIRE(Sample(points, live, 2, 2000.).Pose.HasData);
    REQUIRE_FALSE(Sample(points, live, 2, 2001.).Pose.HasData);
    // SparseBody is an explicit caller policy, never inferred from missing HasData.
    RemotePose::State body;
    std::vector<PosePoint> bodyPoints{Point(body, 1, 1000., TrackedPose(), RemotePose::Policy::SparseBody)};
    bodyPoints.push_back(Point(body, 2, 5000., VRPose{}, RemotePose::Policy::SparseBody));
    REQUIRE(Sample(bodyPoints, body, 2, 999999., RemotePose::Policy::SparseBody).Pose.HasData);
    body.Reset();
    REQUIRE_FALSE(Sample(bodyPoints, body, 2, 999999., RemotePose::Policy::SparseBody).Pose.HasData);
}

TEST_CASE("Duplicate delayed and backward clock packets cannot revive an expired pose", "[remote_pose][remote_pose_state]")
{
    RemotePose::State state;
    const auto pose = TrackedPose();
    const auto original = Point(state, 5000, 1000., pose);
    REQUIRE_FALSE(state.Accept(5000, 5000., pose, RemotePose::Policy::LiveVRPlayer).AcceptedPoint);
    REQUIRE_FALSE(state.Accept(4999, 5000., pose, RemotePose::Policy::LiveVRPlayer).AcceptedPoint);
    REQUIRE_FALSE(state.Accept(1, 5000., pose, RemotePose::Policy::LiveVRPlayer).AcceptedPoint);
    REQUIRE_FALSE(state.Accept(5001, 999., pose, RemotePose::Policy::LiveVRPlayer).AcceptedPoint);
    REQUIRE_FALSE(Sample({original}, state, 5000, 5000.).Pose.HasData);
    // Even a pose-free newer movement establishes a high-water mark.
    Point(state, 6000, 5100., VRPose{});
    REQUIRE_FALSE(state.Accept(5500, 5200., pose, RemotePose::Policy::LiveVRPlayer).AcceptedPoint);
    REQUIRE_FALSE(Sample({original}, state, 5000, 5200.).Pose.HasData);
}

TEST_CASE("Sparse fields survive only within a fresh stream and never carry transient flags", "[remote_pose][remote_pose_state]")
{
    RemotePose::State state;
    auto rich = TrackedPose();
    rich.HasFingers = rich.HasScale = rich.HasWeapons = true;
    rich.RootScale = 1234;
    rich.WeaponHeld = {true, false};
    rich.WeaponOffset[0][0] = 42.f;
    rich.HasRootPosition = rich.HasHips = rich.HasHandCheck = rich.HasSpellAim = true;
    rich.RootPosition[0] = 10.f;
    rich.HipOffset[0] = 20.f;
    Point(state, 1, 1000., rich);
    auto sparse = TrackedPose();
    sparse.NoBones = true;
    const auto kept = Point(state, 2, 1100., sparse);
    REQUIRE(kept.VRPoseData.HasFingers);
    REQUIRE(kept.VRPoseData.RootScale == 1234);
    REQUIRE(kept.VRPoseData.HasWeapons);
    REQUIRE(kept.VRPoseData.WeaponOffset[0][0] == 42.f);
    REQUIRE(kept.VRPoseData.NoBones);
    REQUIRE_FALSE(kept.VRPoseData.HasLegs);
    REQUIRE_FALSE(kept.VRPoseData.HasRootPosition);
    REQUIRE_FALSE(kept.VRPoseData.HasHips);
    REQUIRE_FALSE(kept.VRPoseData.HasHandCheck);
    REQUIRE_FALSE(kept.VRPoseData.HasSpellAim);
    const auto oldGeneration = state.Generation();
    const auto resumed = Point(state, 3, 2101., TrackedPose());
    REQUIRE(state.Generation() != oldGeneration);
    REQUIRE_FALSE(resumed.VRPoseData.HasFingers);
    REQUIRE_FALSE(resumed.VRPoseData.HasScale);
    REQUIRE_FALSE(resumed.VRPoseData.HasWeapons);
    REQUIRE_FALSE(Sample({kept}, state, 2, 2101.).Pose.HasData);
}

TEST_CASE("Session owner and entity replacement invalidate old snapshots", "[remote_pose][remote_pose_state]")
{
    RemotePose::State state;
    REQUIRE(state.Bind({1, 2, 3}));
    const auto original = Point(state, 100, 1000., TrackedPose());
    const auto generation = state.Generation();
    REQUIRE_FALSE(state.Bind({1, 2, 3}));
    REQUIRE(state.Generation() == generation);
    REQUIRE(state.Bind({1, 4, 3}));
    REQUIRE_FALSE(Sample({original}, state, 100, 1001.).Pose.HasData);
    Point(state, 1, 1001., TrackedPose()); // new authenticated owner can start at a lower tick
    REQUIRE(state.Bind({2, 4, 3}));
    REQUIRE(state.Bind({2, 4, 5}));
    const RemotePose::State replacement;
    REQUIRE(replacement.Generation() != state.Generation());
}

TEST_CASE("Pose provenance rejects mismatched ticks future receive times and nonfinite times", "[remote_pose][remote_pose_state]")
{
    RemotePose::State state;
    auto point = Point(state, 10, 1000., TrackedPose());
    REQUIRE_FALSE(Sample({point}, state, 10, 999.).Pose.HasData);
    point.Tick = 11;
    REQUIRE_FALSE(Sample({point}, state, 11, 1001.).Pose.HasData);
    REQUIRE_FALSE(state.Accept(11, std::numeric_limits<double>::infinity(), TrackedPose(), RemotePose::Policy::LiveVRPlayer).AcceptedPoint);
    REQUIRE_FALSE(state.Accept(11, std::numeric_limits<double>::quiet_NaN(), TrackedPose(), RemotePose::Policy::LiveVRPlayer).AcceptedPoint);
    point.Tick = 10;
    REQUIRE_FALSE(Sample({point}, state, 10, std::numeric_limits<double>::quiet_NaN()).Pose.HasData);
    const std::array<PosePoint, 1> array{{point}};
    REQUIRE(RemotePoseInterpolation::SampleWithProvenance(array, 10, state.Generation(), 1001., RemotePose::Policy::LiveVRPlayer).Pose.HasData);
}

TEST_CASE("Recovered or bleeding NPCs relinquish sparse body presentation", "[remote_pose][remote_pose_state]")
{
    CHECK(RemotePose::SparseBodyEligible(true, false, false, false));
    CHECK(RemotePose::SparseBodyEligible(false, true, false, false));
    CHECK_FALSE(RemotePose::SparseBodyEligible(false, false, false, false)); // recovered and standing
    CHECK_FALSE(RemotePose::SparseBodyEligible(false, true, true, false));   // get-up/bleedout animation owns it
    CHECK_FALSE(RemotePose::SparseBodyEligible(true, false, false, true));   // players keep their own death path
}

TEST_CASE("only a pose with bones raises a player copy's playback floor", "[remote_pose]")
{
    // A tracked VR body.
    CHECK(RemotePose::RaisesBodyDelay(true, false, RemotePose::Policy::LiveVRPlayer));
    // A flat player's spell aim: a pose with no bones keeps that player's shorter delay.
    CHECK_FALSE(RemotePose::RaisesBodyDelay(true, true, RemotePose::Policy::LiveVRPlayer));
    // No pose at all.
    CHECK_FALSE(RemotePose::RaisesBodyDelay(false, false, RemotePose::Policy::LiveVRPlayer));
    // A ragdoll's sparse body is not a player's.
    CHECK_FALSE(RemotePose::RaisesBodyDelay(true, false, RemotePose::Policy::SparseBody));
}
