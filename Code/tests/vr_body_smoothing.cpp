#include <catch2/catch.hpp>

#include "../client/Systems/PlaybackClock.h"
#include <Systems/RemotePoseInterpolation.h>

#include <array>
#include <cmath>
#include <vector>

namespace
{
struct PosePoint
{
    uint64_t Tick{};
    VRPose VRPoseData{};
    RemotePose::Provenance PoseSource{};
};

VRPose TurnedPose(const float aDegrees, const float aHand = 0.f)
{
    VRPose pose{};
    pose.HasData = true;
    pose.HasLegs = true;
    for (auto& bone : pose.Bones)
        bone = glm::angleAxis(glm::radians(aDegrees), glm::vec3(0.f, 0.f, 1.f));
    pose.HasHandCheck = true;
    pose.RightHandOffset[0] = aHand;
    pose.LeftHandOffset[1] = -aHand;
    pose.HasFingers = true;
    for (auto& finger : pose.Fingers)
        finger = glm::angleAxis(glm::radians(aDegrees), glm::vec3(1.f, 0.f, 0.f));
    return pose;
}

float DegreesOf(const glm::quat& acRotation)
{
    // Signed turn about z, for the poses above.
    return glm::degrees(2.f * std::atan2(acRotation.z, acRotation.w));
}

PosePoint Point(RemotePose::State& aState, const uint64_t aTick, const double aReceived, const VRPose& acPose)
{
    const auto accepted = aState.Accept(aTick, aReceived, acPose, RemotePose::Policy::LiveVRPlayer);
    REQUIRE(accepted.AcceptedPoint);
    return {aTick, accepted.Pose, accepted.Source};
}
} // namespace

TEST_CASE("A VR body carries on past its newest pose at the speed it last moved", "[vr_body_smoothing]")
{
    const auto cOlder = TurnedPose(0.f, 0.f);
    const auto cNewer = TurnedPose(4.f, 2.f);

    // 40 ms between poses (a headset game at 25 frames), shown 20 ms past the newest: half a step on.
    const auto cPose = RemotePoseInterpolation::Extrapolate(cOlder, 1000, cNewer, 1040, 1060);
    REQUIRE(cPose.HasData);
    for (size_t i = 0; i < VRPose::kBoneCount; ++i)
        REQUIRE(DegreesOf(cPose.Bones[i]) == Approx(6.f).margin(0.01f));
    // Offsets slow down as they are carried: 20 ms of a 100 ms carry-on takes them 18 ms of the last movement on.
    REQUIRE(cPose.RightHandOffset[0] == Approx(2.f + 2.f * 18.f / 40.f));
    REQUIRE(cPose.LeftHandOffset[1] == Approx(-2.f - 2.f * 18.f / 40.f));
    // Grip is a reading, not a motion: the newest one.
    REQUIRE(std::abs(glm::dot(static_cast<glm::quat>(cPose.Fingers[0]), static_cast<glm::quat>(cNewer.Fingers[0]))) == Approx(1.f).margin(1e-4f));
}

TEST_CASE("A VR body's carry-on is capped in time and in angle", "[vr_body_smoothing]")
{
    const auto cOlder = TurnedPose(0.f, 0.f);
    const auto cNewer = TurnedPose(4.f, 2.f);

    // Past 100 ms it holds where 100 ms took it.
    const auto cFar = RemotePoseInterpolation::Extrapolate(cOlder, 1000, cNewer, 1040, 1040 + 500);
    REQUIRE(DegreesOf(cFar.Bones[0]) == Approx(4.f + 10.f).margin(0.01f));
    // An offset has slowed to a stop by then, half as far on as at full speed.
    REQUIRE(cFar.RightHandOffset[0] == Approx(2.f + 2.5f));

    // A fast swing turns no bone more than 30 degrees past the newest pose.
    const auto cFast = RemotePoseInterpolation::Extrapolate(TurnedPose(0.f), 1000, TurnedPose(40.f), 1040, 1140);
    REQUIRE(DegreesOf(cFast.Bones[0]) == Approx(70.f).margin(0.01f));

    // Poses more than 250 ms apart, or none older, say nothing about the motion now: the newest is held.
    const auto cStale = RemotePoseInterpolation::Extrapolate(cOlder, 1000, cNewer, 1300, 1350);
    REQUIRE(DegreesOf(cStale.Bones[0]) == Approx(4.f).margin(0.01f));
    const auto cOff = RemotePoseInterpolation::Extrapolate(cOlder, 1000, cNewer, 1040, 1060, 0.0);
    REQUIRE(DegreesOf(cOff.Bones[0]) == Approx(4.f).margin(0.01f));
    const auto cAtNewest = RemotePoseInterpolation::Extrapolate(cOlder, 1000, cNewer, 1040, 1040);
    REQUIRE(DegreesOf(cAtNewest.Bones[0]) == Approx(4.f).margin(0.01f));
}

TEST_CASE("A VR body's carry-on keeps the short arc and boneless poses", "[vr_body_smoothing]")
{
    // From 170 to -170 degrees is a 20 degree turn through 180, not 340 the other way.
    const auto cPose = RemotePoseInterpolation::Extrapolate(TurnedPose(170.f), 1000, TurnedPose(-170.f), 1040, 1060);
    REQUIRE(std::abs(glm::dot(static_cast<glm::quat>(cPose.Bones[0]), glm::angleAxis(glm::radians(-160.f), glm::vec3(0.f, 0.f, 1.f)))) == Approx(1.f).margin(1e-4f));

    auto boneless = TurnedPose(4.f, 2.f);
    boneless.NoBones = true;
    const auto cHeld = RemotePoseInterpolation::Extrapolate(TurnedPose(0.f), 1000, boneless, 1040, 1060);
    REQUIRE(cHeld.NoBones);
    REQUIRE(DegreesOf(cHeld.Bones[0]) == Approx(4.f).margin(0.01f));
    // Hands still carry on, slowing down: 18 ms of the last movement 20 ms into a 100 ms carry-on.
    REQUIRE(cHeld.RightHandOffset[0] == Approx(2.f + 2.f * 18.f / 40.f));
}

TEST_CASE("The receiver carries a VR body on only when asked, and keeps the newest pair's provenance", "[vr_body_smoothing]")
{
    RemotePose::State state;
    const std::vector<PosePoint> points{Point(state, 1000, 5000., TurnedPose(0.f)), Point(state, 1040, 5040., TurnedPose(4.f))};

    const auto cHeld = RemotePoseInterpolation::SampleWithProvenance(points, 1060, state.Generation(), 5070., RemotePose::Policy::LiveVRPlayer);
    REQUIRE(DegreesOf(cHeld.Pose.Bones[0]) == Approx(4.f).margin(0.01f));
    REQUIRE(cHeld.Before.SourceTick == 1040);

    const auto cCarried = RemotePoseInterpolation::SampleWithProvenance(
        points, 1060, state.Generation(), 5070., RemotePose::Policy::LiveVRPlayer, RemotePose::kLiveTimeoutMs, RemotePoseInterpolation::kMaxPoseExtrapolationMs);
    REQUIRE(DegreesOf(cCarried.Pose.Bones[0]) == Approx(6.f).margin(0.01f));
    REQUIRE(cCarried.Before.SourceTick == 1000);
    REQUIRE(cCarried.After.SourceTick == 1040);
    REQUIRE(cCarried.After.ReceivedAtMs == 5040.);
    REQUIRE(cCarried.Generation == state.Generation());

    // Between two poses it still blends, as before.
    const auto cBetween = RemotePoseInterpolation::SampleWithProvenance(
        points, 1020, state.Generation(), 5070., RemotePose::Policy::LiveVRPlayer, RemotePose::kLiveTimeoutMs, RemotePoseInterpolation::kMaxPoseExtrapolationMs);
    REQUIRE(DegreesOf(cBetween.Pose.Bones[0]) == Approx(2.f).margin(0.01f));

    // One pose alone is held.
    const std::vector<PosePoint> single{points.back()};
    const auto cSingle = RemotePoseInterpolation::SampleWithProvenance(
        single, 1060, state.Generation(), 5070., RemotePose::Policy::LiveVRPlayer, RemotePose::kLiveTimeoutMs, RemotePoseInterpolation::kMaxPoseExtrapolationMs);
    REQUIRE(DegreesOf(cSingle.Pose.Bones[0]) == Approx(4.f).margin(0.01f));
}

TEST_CASE("A playback clock holds a VR body at least its floor behind", "[vr_body_smoothing]")
{
    // A steady 40 ms sender with no jitter wants about 48 ms of delay; the floor asks for 100.
    const auto cRun = [](const double aFloor)
    {
        PlaybackClock clock;
        if (aFloor > 0.0)
            clock.SetMinTargetDelay(aFloor);
        for (int i = 0; i < 100; ++i)
            clock.AddSample(10.0 + i * 40.0, 1'000'000 + static_cast<uint64_t>(i) * 40);
        return clock.GetTargetDelay();
    };
    REQUIRE(cRun(0.0) < 60.0);
    REQUIRE(cRun(100.0) == Approx(100.0));
    // Out of range or not a number: clamped, or left as it was.
    REQUIRE(cRun(10'000.0) == Approx(PlaybackClock::cMaxTargetDelay));
    REQUIRE(cRun(std::nan("")) < 60.0);
}

TEST_CASE("A VR body's hands are carried on no further than the cap", "[vr_body_smoothing]")
{
    // A 20 ms pair with 5 units of travel: at full speed, 100 ms would carry it 25 units on.
    const auto cOlder = TurnedPose(0.f, 0.f);
    const auto cNewer = TurnedPose(0.f, 5.f);
    for (uint64_t ahead = 0; ahead <= 300; ahead += 10)
    {
        const auto cPose = RemotePoseInterpolation::Extrapolate(cOlder, 1000, cNewer, 1020, 1020 + ahead);
        REQUIRE(cPose.RightHandOffset[0] - 5.f <= RemotePoseInterpolation::kMaxCarryUnits + 1e-4f);
        REQUIRE(cPose.RightHandOffset[0] >= 5.f);
    }
    // The cap is on the offset's whole move, not each axis.
    VRPose diagonal = cNewer;
    diagonal.RightHandOffset[1] = 5.f;
    const auto cDiagonal = RemotePoseInterpolation::Extrapolate(cOlder, 1000, diagonal, 1020, 1120);
    const float cMoved = std::hypot(cDiagonal.RightHandOffset[0] - 5.f, cDiagonal.RightHandOffset[1] - 5.f);
    REQUIRE(cMoved == Approx(RemotePoseInterpolation::kMaxCarryUnits));
}

TEST_CASE("A VR body carried on blends back to the late pose instead of snapping", "[vr_body_smoothing]")
{
    REQUIRE(RemotePoseInterpolation::CarriedMs(0.0, 100.0) == 0.0);
    REQUIRE(RemotePoseInterpolation::CarriedMs(100.0, 100.0) == Approx(50.0));
    REQUIRE(RemotePoseInterpolation::CarriedMs(500.0, 100.0) == Approx(50.0));
    REQUIRE(RemotePoseInterpolation::CarriedMs(50.0, 0.0) == 0.0);

    REQUIRE(RemotePoseInterpolation::BlendBackWeight(0.0) == 1.f);
    REQUIRE(RemotePoseInterpolation::BlendBackWeight(RemotePoseInterpolation::kPoseBlendBackMs / 2.0) == Approx(0.5f));
    REQUIRE(RemotePoseInterpolation::BlendBackWeight(RemotePoseInterpolation::kPoseBlendBackMs) == 0.f);
    STATIC_REQUIRE(RemotePoseInterpolation::kPoseBlendBackMs >= 60.0);
    STATIC_REQUIRE(RemotePoseInterpolation::kPoseBlendBackMs <= 80.0);

    const auto cShown = TurnedPose(20.f, 12.f);
    const auto cLate = TurnedPose(0.f, 0.f);
    const auto cHalf = RemotePoseInterpolation::Blend(cShown, cLate, 0.5f);
    REQUIRE(DegreesOf(cHalf.Bones[0]) == Approx(10.f).margin(0.05f));
    REQUIRE(cHalf.RightHandOffset[0] == Approx(6.f).margin(0.01f));
    REQUIRE(DegreesOf(RemotePoseInterpolation::Blend(cShown, cLate, 0.f).Bones[0]) == Approx(20.f).margin(0.01f));
    const auto cDone = RemotePoseInterpolation::Blend(cShown, cLate, 1.f);
    REQUIRE(DegreesOf(cDone.Bones[0]) == Approx(0.f).margin(0.01f));
    REQUIRE(cDone.RightHandOffset[0] == Approx(0.f));
    // Grip is the late pose's reading.
    REQUIRE(std::abs(glm::dot(static_cast<glm::quat>(cHalf.Fingers[0]), static_cast<glm::quat>(cLate.Fingers[0]))) == Approx(1.f).margin(1e-4f));
}

namespace
{
// One receiver frame as PublishPose draws it: the sample at aTick, carried on up to the cap, blended after the last shown.
struct Receiver
{
    RemotePose::State State;
    std::vector<PosePoint> Points;
    RemotePoseInterpolation::BlendBack Blend;
    RemotePose::Sample Shown;
    RemotePose::Sample Raw;

    void Receive(const uint64_t aTick, const float aDegrees) { Points.push_back(Point(State, aTick, 4000.0 + static_cast<double>(aTick), TurnedPose(aDegrees))); }

    float Frame(const uint64_t aTick, const double aNowMs)
    {
        Raw = RemotePoseInterpolation::SampleWithProvenance(
            Points, aTick, State.Generation(), aNowMs, RemotePose::Policy::LiveVRPlayer, RemotePose::kLiveTimeoutMs, RemotePoseInterpolation::kMaxPoseExtrapolationMs);
        RemotePose::Sample sample = Raw;
        RemotePoseInterpolation::Present(Blend, Shown, sample, aTick, aNowMs);
        Shown = sample;
        return DegreesOf(Shown.Pose.Bones[0]);
    }
};
} // namespace

TEST_CASE("A late pose that comes while the body is still carried on blends from the pose shown", "[vr_body_smoothing]")
{
    Receiver receiver;
    receiver.Receive(1000, 0.f);
    receiver.Receive(1040, 4.f);
    REQUIRE(receiver.Frame(1050, 5050.0) == Approx(5.f).margin(0.01f));
    REQUIRE(receiver.Frame(1060, 5060.0) == Approx(6.f).margin(0.01f));
    REQUIRE(receiver.Frame(1070, 5070.0) == Approx(7.f).margin(0.01f));

    // It turned slower than it was carried on, and the pose says so only after the body was shown past it: the new pair
    // is carried on too. The body stays where it was shown instead of jumping back to 6.25 degrees.
    receiver.Receive(1080, 6.f);
    REQUIRE(receiver.Frame(1085, 5085.0) == Approx(7.f).margin(0.01f));
    REQUIRE(RemotePoseInterpolation::IsCarried(receiver.Raw, 1085));
    REQUIRE(DegreesOf(receiver.Raw.Pose.Bones[0]) == Approx(6.25f).margin(0.01f));

    // On the way to the new pair's carry-on, then on it once the blend is over.
    const float cEarly = receiver.Frame(1095, 5095.0);
    REQUIRE(cEarly < 7.f);
    REQUIRE(cEarly > DegreesOf(receiver.Raw.Pose.Bones[0]));
    REQUIRE(receiver.Frame(1155, 5155.0) == Approx(9.75f).margin(0.01f));
    REQUIRE(receiver.Blend.Since < 0.0);
}

TEST_CASE("A second late pose during the blend back blends on from the pose shown", "[vr_body_smoothing]")
{
    Receiver receiver;
    receiver.Receive(1000, 0.f);
    receiver.Receive(1040, 4.f);
    for (uint64_t tick = 1050; tick <= 1070; tick += 10)
        receiver.Frame(tick, 4000.0 + static_cast<double>(tick));
    receiver.Receive(1080, 6.f);
    receiver.Frame(1085, 5085.0);
    receiver.Frame(1095, 5095.0);
    const float cShown = receiver.Frame(1105, 5105.0);

    // Another pose, late again, 20 ms into the 70 ms return: no jump to its own carry-on (7.75 degrees).
    receiver.Receive(1100, 7.f);
    REQUIRE(receiver.Frame(1115, 5115.0) == Approx(cShown).margin(0.01f));
    REQUIRE(DegreesOf(receiver.Raw.Pose.Bones[0]) == Approx(7.75f).margin(0.01f));
    REQUIRE(receiver.Blend.Since == 5115.0);
    // The blend ends 70 ms after the newest late pose.
    receiver.Frame(1115 + 60, 5115.0 + 60.0);
    REQUIRE(receiver.Blend.Since >= 0.0);
    REQUIRE(receiver.Frame(1115 + 70, 5115.0 + 70.0) == Approx(DegreesOf(receiver.Raw.Pose.Bones[0])).margin(0.01f));
    REQUIRE(receiver.Blend.Since < 0.0);
}

TEST_CASE("Running past the newest pose again during the blend back does not cut it short", "[vr_body_smoothing]")
{
    Receiver receiver;
    receiver.Receive(1000, 0.f);
    receiver.Receive(1040, 4.f);
    receiver.Frame(1050, 5050.0);
    REQUIRE(receiver.Frame(1060, 5060.0) == Approx(6.f).margin(0.01f));

    // A pose comes in time to play between, but the body was carried on to 6 degrees: it blends back from there.
    receiver.Receive(1080, 2.f);
    REQUIRE(receiver.Frame(1070, 5080.0) == Approx(6.f).margin(0.01f));
    const float cAtPose = receiver.Frame(1080, 5090.0);
    // The render clock passes that pose before the next comes: carried on again, from the pair it blends to.
    const float cCarried = receiver.Frame(1090, 5100.0);
    REQUIRE(RemotePoseInterpolation::IsCarried(receiver.Raw, 1090));
    REQUIRE(std::abs(cCarried - cAtPose) < 1.f);
    REQUIRE(cCarried > DegreesOf(receiver.Raw.Pose.Bones[0]) + 3.f);
    REQUIRE(receiver.Blend.Since == 5080.0);
}

TEST_CASE("A new pose stream or an empty pose ends the blend back", "[vr_body_smoothing]")
{
    RemotePoseInterpolation::BlendBack blend;
    blend.From = TurnedPose(20.f);
    blend.Since = 1000.0;
    blend.Generation = 1;

    RemotePose::Sample sample;
    sample.Pose = TurnedPose(0.f);
    sample.Generation = 2;
    RemotePoseInterpolation::Present(blend, RemotePose::Sample{}, sample, 0, 1010.0);
    REQUIRE(DegreesOf(sample.Pose.Bones[0]) == Approx(0.f).margin(0.01f));
    REQUIRE(blend.Since < 0.0);

    blend.Since = 1000.0;
    RemotePose::Sample empty;
    empty.Generation = 1;
    RemotePoseInterpolation::Present(blend, RemotePose::Sample{}, empty, 0, 1010.0);
    REQUIRE_FALSE(empty.Pose.HasData);
    REQUIRE(blend.Since < 0.0);

    // Not carried on before: a changed pair is ordinary playback and starts no blend.
    RemotePoseInterpolation::BlendBack idle;
    RemotePose::Sample shown;
    shown.Pose = TurnedPose(5.f);
    shown.Generation = 1;
    RemotePose::Sample next = sample;
    next.Generation = 1;
    next.Before.Valid = true;
    next.Before.SourceTick = 40;
    RemotePoseInterpolation::Present(idle, shown, next, 50, 1010.0);
    REQUIRE(idle.Since < 0.0);
    REQUIRE(DegreesOf(next.Pose.Bones[0]) == Approx(0.f).margin(0.01f));
}
