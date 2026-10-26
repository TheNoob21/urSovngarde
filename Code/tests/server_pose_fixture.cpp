#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>
#include <catch2/catch.hpp>
#include <Messages/ClientReferencesMoveRequest.h>
#include <Messages/ServerReferencesMoveRequest.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <type_traits>
#include "../bot/PoseFixture.h"

namespace
{
template <class T> T RoundTrip(const T& acMessage)
{
    TiltedPhoques::Buffer bytes(16384);
    TiltedPhoques::Buffer::Writer writer(&bytes);
    acMessage.Serialize(writer);
    TiltedPhoques::Buffer::Reader reader(&bytes);
    if constexpr (std::is_base_of_v<ClientMessage, T>)
    {
        auto decoded = ClientMessageFactory{}.Extract(reader);
        REQUIRE(decoded);
        REQUIRE(decoded->GetOpcode() == T::Opcode);
        return *TiltedPhoques::CastUnique<T>(std::move(decoded));
    }
    else
    {
        auto decoded = ServerMessageFactory{}.Extract(reader);
        REQUIRE(decoded);
        REQUIRE(decoded->GetOpcode() == T::Opcode);
        return *TiltedPhoques::CastUnique<T>(std::move(decoded));
    }
}
}

// These test the production-used bot fixture and its receive assertion guard.
// Actual server relay is covered separately by mixed-runtime-smoke.ps1.
TEST_CASE("Server pose fixtures survive both movement wire directions", "[server.pose.fixture]")
{
    using namespace PoseFixture;
    const auto a = Make(Phase::A);
    const auto b = Make(Phase::B);
    const auto recovery = Make(Phase::Recovery);
    REQUIRE(a != b);
    REQUIRE(a != recovery);
    REQUIRE(b != recovery);
    for (const auto phase : {Phase::A, Phase::B, Phase::Recovery})
    {
        const auto pose = Make(phase);
        CHECK(pose.HasData);
        CHECK_FALSE(pose.NoBones);
        CHECK(pose.HasLegs);
        CHECK(pose.HasFingers);
        CHECK(pose.HasHips);
        CHECK(pose.HasHandCheck);
        CHECK(pose.HasScale);
        CHECK(pose.HasWeapons);
        Quaternion_NetQuantize identity;
        identity = glm::quat(1.f, 0.f, 0.f, 0.f);
        CHECK(pose.Bones[VRPose::kHead] != identity);
        ClientReferencesMoveRequest outgoing;
        outgoing.Tick = 1234;
        outgoing.Updates[42].UpdatedVRPose = pose;
        const auto atServer = RoundTrip(outgoing);
        REQUIRE(atServer.Updates.size() == 1);
        REQUIRE(atServer.Updates.count(42) == 1);
        CHECK(atServer.Updates.at(42).UpdatedVRPose == pose);
        ServerReferencesMoveRequest relayed;
        relayed.Tick = outgoing.Tick;
        relayed.Updates[42] = atServer.Updates.at(42);
        const auto received = RoundTrip(relayed);
        REQUIRE(received.Updates.count(42) == 1);
        Observation observation;
        observation.Receive(received.Updates.at(42).UpdatedVRPose);
        CHECK(observation.Matches(phase, 0));
        CHECK_FALSE(observation.Matches(phase, observation.Count));
    }
    CHECK(a.Bones[VRPose::kHead] != b.Bones[VRPose::kHead]);
    CHECK(a.Bones[VRPose::kLeftUpperArm] != b.Bones[VRPose::kLeftUpperArm]);
    CHECK(a.Bones[VRPose::kRightUpperArm] != b.Bones[VRPose::kRightUpperArm]);
    CHECK(a.RootScale != b.RootScale);
    CHECK(a.HipOffset[0] != b.HipOffset[0]);
    CHECK(a.LeftHandOffset[0] != b.LeftHandOffset[0]);
    CHECK(a.RightHandOffset[0] != b.RightHandOffset[0]);

    ClientReferencesMoveRequest noData;
    noData.Updates[42].UpdatedVRPose = Make(Phase::NoData);
    const auto atServer = RoundTrip(noData);
    ServerReferencesMoveRequest relayed;
    relayed.Updates[42] = atServer.Updates.at(42);
    const auto received = RoundTrip(relayed);
    Observation transition;
    transition.Receive(a);
    const auto beforeNoData = transition.Count;
    transition.Receive(received.Updates.at(42).UpdatedVRPose);
    CHECK(transition.Matches(Phase::NoData, beforeNoData));
}

TEST_CASE("Server pose assertion rejects defaults stale data and field corruption", "[server.pose.fixture]")
{
    using namespace PoseFixture;
    Observation observed;
    CHECK_FALSE(observed.Matches(Phase::NoData, 0));
    observed.Receive(Make(Phase::NoData));
    CHECK_FALSE(observed.Matches(Phase::NoData, 0)); // no preceding actual pose
    observed.Receive(Make(Phase::A));
    const auto mark = observed.Count;
    CHECK_FALSE(observed.Matches(Phase::A, mark));
    CHECK_FALSE(observed.Matches(Phase::B, 0));
    observed.Receive(Make(Phase::NoData));
    CHECK(observed.Matches(Phase::NoData, mark));
    observed = {}; // disconnect discards the actor and its entire observation
    CHECK_FALSE(observed.Matches(Phase::NoData, 0));
    CHECK_FALSE(observed.Matches(Phase::A, 0));
    observed.Receive(Make(Phase::Recovery));
    CHECK(observed.Matches(Phase::Recovery, 0));
    CHECK_FALSE(observed.Matches(Phase::A, 0));

    const auto pose = Make(Phase::A);
    for (int field = 0; field < 11; ++field)
    {
        auto corrupt = pose;
        switch (field)
        {
        case 0: corrupt.HasData = false; break;
        case 1: corrupt.Bones[VRPose::kHead] = Make(Phase::B).Bones[VRPose::kHead]; break;
        case 2: corrupt.Bones[VRPose::kLeftUpperArm] = Make(Phase::B).Bones[VRPose::kLeftUpperArm]; break;
        case 3: corrupt.Bones[VRPose::kRightUpperArm] = Make(Phase::B).Bones[VRPose::kRightUpperArm]; break;
        case 4: corrupt.HipOffset[1] += 1.f; break;
        case 5: ++corrupt.RootScale; break;
        case 6: corrupt.LeftHandOffset[2] += 1.f; break;
        case 7: corrupt.RightHandOffset[0] += 1.f; break;
        case 8: corrupt.Fingers[7] = Make(Phase::B).Fingers[7]; break;
        case 9: corrupt.WeaponOffset[1][2] += 1.f; break;
        case 10: corrupt.HasHips = false; break;
        }
        observed.Receive(corrupt);
        CHECK_FALSE(observed.Matches(Phase::A, 0));
    }
    CHECK_FALSE(Parse("invalid").has_value());
}
