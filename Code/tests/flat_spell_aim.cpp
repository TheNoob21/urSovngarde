#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <Games/Skyrim/SpellAimRules.h>
#include <Structs/ReferenceUpdate.h>
#include <Systems/RemotePoseInterpolation.h>

#include <limits>
#include <vector>

namespace
{
constexpr auto kLive = RemotePose::Policy::LiveVRPlayer;

void CheckAimOnly(const VRPose& acPose)
{
    REQUIRE(acPose.HasData);
    REQUIRE(acPose.NoBones);
    CHECK_FALSE(acPose.HasLegs);
    CHECK_FALSE(acPose.HasFingers);
    CHECK_FALSE(acPose.HasScale);
    CHECK_FALSE(acPose.HasRootPosition);
    CHECK_FALSE(acPose.HasHips);
    CHECK_FALSE(acPose.HasHandCheck);
    CHECK_FALSE(acPose.HasWeapons);
    CHECK_FALSE(SpellAimRules::NeedsBodyPresentation(acPose.NoBones, acPose.HasRootPosition));
}

VRPose Aim(float aHeading, float aPitch)
{
    SpellAimRules::LocalAim aim;
    aim.Note(aHeading, aPitch, 0.0);
    return aim.AimOnly(0.0);
}

struct Point
{
    uint64_t Tick{};
    VRPose VRPoseData{};
    RemotePose::Provenance PoseSource{};
};

Point Accept(RemotePose::State& aState, uint64_t aTick, double aReceived, const VRPose& acPose)
{
    const auto accepted = aState.Accept(aTick, aReceived, acPose, kLive);
    REQUIRE(accepted.AcceptedPoint);
    return {aTick, accepted.Pose, accepted.Source};
}
} // namespace

TEST_CASE("Flat held spell pairs only this update's local finite call results", "[spellaim]")
{
    SpellAimRules::Pair pair;
    float heading = 99.f;
    CHECK_FALSE(pair.Pitch(true, .2f, 0.0, heading));
    pair.Heading(true, 1.f, 10.0);
    REQUIRE(pair.Pitch(true, -.2f, 35.0, heading)); // inclusive 25ms limit
    CHECK(heading == 1.f);
    CHECK_FALSE(pair.Pitch(true, .3f, 35.0, heading)); // consumed

    for (const double now : {9.0, 35.001, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
    {
        pair.Heading(true, 1.f, 10.0);
        CHECK_FALSE(pair.Pitch(true, .2f, now, heading));
        CHECK_FALSE(pair.Pitch(true, .2f, 10.0, heading)); // rejected pairs are consumed too
    }
    for (const float bad : {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
    {
        pair.Heading(true, bad, 10.0);
        CHECK_FALSE(pair.Pitch(true, .2f, 10.0, heading));
        pair.Heading(true, 1.f, 10.0);
        CHECK_FALSE(pair.Pitch(true, bad, 10.0, heading));
        CHECK_FALSE(SpellAimRules::Finite(bad, .2f));
        CHECK_FALSE(SpellAimRules::Finite(1.f, bad));
    }
    pair.Heading(false, 1.f, 10.0); // NPC or replay
    CHECK_FALSE(pair.Pitch(true, .2f, 10.0, heading));
    pair.Heading(true, 1.f, std::numeric_limits<double>::infinity());
    CHECK_FALSE(pair.Pitch(true, .2f, 10.0, heading));
    pair.Heading(true, 1.f, 10.0);
    CHECK_FALSE(pair.Pitch(false, .2f, 10.0, heading));
    pair.Heading(true, 1.f, 10.0);
    pair.Heading(false, 2.f, 10.0); // intervening NPC cannot provide half a local pair
    CHECK_FALSE(pair.Pitch(true, .2f, 10.0, heading));

    SpellAimRules::Pair otherKindOrThread;
    pair.Heading(true, 1.f, 10.0);
    CHECK_FALSE(otherKindOrThread.Pitch(true, .2f, 10.0, heading));
    REQUIRE(pair.Pitch(true, .2f, 10.0, heading));
}

TEST_CASE("Flat aim snapshots expire and never carry a body", "[spellaim]")
{
    SpellAimRules::LocalAim aim;
    const auto idle = aim.AimOnly(0.0);
    CheckAimOnly(idle);
    CHECK_FALSE(idle.HasSpellAim);
    aim.Note(1.5f, -.25f, 100.0);
    auto current = aim.AimOnly(500.0);
    CheckAimOnly(current);
    REQUIRE(current.HasSpellAim);
    CHECK(current.SpellAim[0] == 1.5f);
    CHECK(current.SpellAim[1] == -.25f);
    for (const double now : {99.0, 500.001, std::numeric_limits<double>::quiet_NaN()})
    {
        const auto expired = aim.AimOnly(now);
        CheckAimOnly(expired);
        CHECK_FALSE(expired.HasSpellAim);
        CHECK(expired.SpellAim[0] == 0.f);
        CHECK(expired.SpellAim[1] == 0.f);
    }
    // Bad observations cannot renew the last good observation's time.
    aim.Note(std::numeric_limits<float>::quiet_NaN(), 1.f, 500.0);
    aim.Note(1.f, std::numeric_limits<float>::infinity(), 500.0);
    aim.Note(1.f, 1.f, std::numeric_limits<double>::infinity());
    CHECK_FALSE(aim.AimOnly(501.0).HasSpellAim);

    // The same local cache fills a tracked VR pose without touching its body fields.
    VRPose tracked{};
    tracked.HasData = tracked.HasLegs = tracked.HasRootPosition = tracked.HasWeapons = true;
    tracked.RootPosition[0] = 123.f;
    tracked.Bones[0] = glm::quat(1.f, 0.f, 0.f, 0.f);
    aim.Fill(tracked, 100.0);
    CHECK(tracked.HasSpellAim);
    CHECK(tracked.HasData);
    CHECK_FALSE(tracked.NoBones);
    CHECK(tracked.HasLegs);
    CHECK(tracked.HasRootPosition);
    CHECK(tracked.HasWeapons);
    CHECK(tracked.RootPosition[0] == 123.f);
    CHECK(static_cast<glm::quat>(tracked.Bones[0]).w == Approx(1.f));
    CHECK(SpellAimRules::NeedsBodyPresentation(tracked.NoBones, tracked.HasRootPosition));
    CHECK(SpellAimRules::NeedsBodyPresentation(false, false)); // tracked VR bones need no root-position field
    CHECK(SpellAimRules::NeedsBodyPresentation(true, true));   // existing boneless dragged body
}

TEST_CASE("Flat aim travels in the existing ReferenceUpdate and absent aim clears reuse", "[spellaim][wire.contract]")
{
    ReferenceUpdate sent{};
    sent.UpdatedMovement.CellId = GameId(0, 0x1234);
    sent.UpdatedVRPose = Aim(1.5f, -.25f);
    ReferenceUpdate back{};
    for (const bool present : {true, false, true})
    {
        sent.UpdatedVRPose.HasSpellAim = present;
        TiltedPhoques::Buffer buffer(1024);
        TiltedPhoques::Buffer::Writer writer(&buffer);
        sent.Serialize(writer);
        TiltedPhoques::Buffer::Reader reader(&buffer);
        back.Deserialize(reader);
        REQUIRE(back == sent);
        CheckAimOnly(back.UpdatedVRPose);
        REQUIRE(back.UpdatedVRPose.HasSpellAim == present);
        if (present)
        {
            CHECK(back.UpdatedVRPose.SpellAim[0] == 1.5f);
            CHECK(back.UpdatedVRPose.SpellAim[1] == -.25f);
        }
    }
}

TEST_CASE("Flat aim follows live receive provenance, clears and wraps without a body", "[spellaim][remote_pose]")
{
    RemotePose::State state;
    REQUIRE(state.Bind({1, 2, 3}));
    std::vector<Point> points;
    points.push_back(Accept(state, 10, 100.0, Aim(glm::radians(179.f), -.5f)));
    points.push_back(Accept(state, 20, 110.0, Aim(glm::radians(-179.f), .5f)));
    auto sample = RemotePoseInterpolation::SampleWithProvenance(points, 15, state.Generation(), 120.0, kLive);
    CheckAimOnly(sample.Pose);
    REQUIRE(sample.Pose.HasSpellAim);
    CHECK(sample.Pose.SpellAim[0] == Approx(glm::pi<float>()));
    CHECK(sample.Pose.SpellAim[1] == Approx(0.f));
    CHECK(sample.Before.ReceivedAtMs == 100.0);
    CHECK(sample.After.ReceivedAtMs == 110.0);
    CHECK_FALSE(state.Accept(20, 120.0, Aim(0.f, 0.f), kLive).AcceptedPoint);
    CHECK_FALSE(state.Accept(19, 120.0, Aim(0.f, 0.f), kLive).AcceptedPoint);

    SpellAimRules::LocalAim idle;
    points.push_back(Accept(state, 30, 120.0, idle.AimOnly(120.0)));
    sample = RemotePoseInterpolation::SampleWithProvenance(points, 30, state.Generation(), 120.0, kLive);
    CheckAimOnly(sample.Pose);
    CHECK_FALSE(sample.Pose.HasSpellAim); // a real empty sample, not missing tracking
    sample = RemotePoseInterpolation::SampleWithProvenance(points, 30, state.Generation(), 1120.001, kLive);
    CHECK_FALSE(sample.Pose.HasData); // holding a render endpoint cannot extend the receive deadline

    const auto oldGeneration = state.Generation();
    REQUIRE(state.Bind({1, 9, 3})); // same actor id, new trusted owner
    CHECK(state.Generation() != oldGeneration);
    sample = RemotePoseInterpolation::SampleWithProvenance(points, 20, state.Generation(), 120.0, kLive);
    CHECK_FALSE(sample.Pose.HasData);
    points.clear(); // trusted lifecycle caller also clears its queue
    points.push_back(Accept(state, 1, 121.0, Aim(.2f, .3f)));
    sample = RemotePoseInterpolation::SampleWithProvenance(points, 1, state.Generation(), 121.0, kLive);
    REQUIRE(sample.Pose.HasSpellAim);
    CHECK(sample.Pose.SpellAim[0] == .2f);
    REQUIRE(state.Bind({2, 9, 3})); // reconnect is another generation
    CHECK_FALSE(RemotePoseInterpolation::SampleWithProvenance(points, 1, state.Generation(), 122.0, kLive).Pose.HasData);
}

TEST_CASE("Flat aim is sent while a spell is held and once more when it ends", "[spellaim]")
{
    SpellAimRules::LocalAim aim;
    VRPose pose{};
    CHECK_FALSE(aim.Capture(pose, 0.0)); // nothing cast: no snapshot on the wire
    aim.Note(1.f, .5f, 100.0);
    REQUIRE(aim.Capture(pose, 150.0));
    CHECK(pose.HasSpellAim);
    CHECK(aim.Capture(pose, 200.0));
    REQUIRE(aim.Capture(pose, 600.0)); // aim expired: one clearing snapshot
    CHECK_FALSE(pose.HasSpellAim);
    CheckAimOnly(pose);
    CHECK_FALSE(aim.Capture(pose, 700.0));
    aim.Note(2.f, .1f, 800.0);
    CHECK(aim.Capture(pose, 810.0));
}

TEST_CASE("A new session sends no aim from the old one", "[spellaim]")
{
    SpellAimRules::LocalAim aim;
    VRPose pose{};
    aim.Note(1.f, .5f, 100.0);
    REQUIRE(aim.Capture(pose, 150.0));
    aim.Reset(); // within the aim's lifetime
    CHECK_FALSE(aim.Capture(pose, 200.0));
    CHECK_FALSE(aim.AimOnly(200.0).HasSpellAim);

    aim.Note(1.f, .5f, 300.0);
    REQUIRE(aim.Capture(pose, 310.0));
    aim.Reset(); // after expiry the old session's pending clear is gone too
    CHECK_FALSE(aim.Capture(pose, 900.0));
    aim.Note(2.f, .1f, 1000.0);
    REQUIRE(aim.Capture(pose, 1010.0));
    CHECK(pose.SpellAim[0] == 2.f);
}

TEST_CASE("A cast made offline is never noted", "[spellaim]")
{
    CHECK(SpellAimRules::CanNote(true, true));
    CHECK_FALSE(SpellAimRules::CanNote(true, false));
    CHECK_FALSE(SpellAimRules::CanNote(false, true));

    // Cast online, disconnect, cast offline, reconnect: the first movement carries no aim.
    SpellAimRules::LocalAim aim;
    SpellAimRules::Pair pair;
    VRPose pose{};
    float heading = 0.f;
    pair.Heading(SpellAimRules::CanNote(true, true), 1.f, 10.0);
    REQUIRE(pair.Pitch(SpellAimRules::CanNote(true, true), .5f, 10.0, heading));
    aim.Note(heading, .5f, 10.0);
    aim.Reset(); // disconnect
    pair.Heading(SpellAimRules::CanNote(true, false), 2.f, 20.0);
    CHECK_FALSE(pair.Pitch(SpellAimRules::CanNote(true, false), .5f, 20.0, heading));
    CHECK_FALSE(aim.Capture(pose, 30.0)); // reconnected within the aim's lifetime
}
