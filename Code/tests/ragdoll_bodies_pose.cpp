#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <catch2/catch.hpp>

#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/RequestRagdollPose.h>
#include <Messages/NotifyRagdollPose.h>

#include <Games/Skyrim/CopyBodyRules.h>
#include <Games/Skyrim/RagdollBodiesPose.h>
#include <RagdollPoseRelay.h>

#include <cmath>
#include <limits>
#include <vector>

using namespace TiltedPhoques;
using namespace RagdollBodiesPose;
using namespace std::chrono_literals;

namespace
{
template <class T> T ClientRoundTrip(const T& acMessage)
{
    Buffer buffer(1024);
    Buffer::Writer writer(&buffer);
    acMessage.Serialize(writer);
    Buffer::Reader reader(&buffer);
    auto pMessage = ClientMessageFactory{}.Extract(reader);
    REQUIRE(pMessage);
    REQUIRE(pMessage->GetOpcode() == T::Opcode);
    return *CastUnique<T>(std::move(pMessage));
}

template <class T> T ServerRoundTrip(const T& acMessage)
{
    Buffer buffer(1024);
    Buffer::Writer writer(&buffer);
    acMessage.Serialize(writer);
    Buffer::Reader reader(&buffer);
    auto pMessage = ServerMessageFactory{}.Extract(reader);
    REQUIRE(pMessage);
    REQUIRE(pMessage->GetOpcode() == T::Opcode);
    return *CastUnique<T>(std::move(pMessage));
}

// A wolf-sized ragdoll lying near a far corner of the world, each body turned its own way.
std::vector<Body> Wolf(const glm::vec3& acRoot, const size_t aCount = 12)
{
    std::vector<Body> bodies(aCount);
    for (size_t i = 0; i < aCount; ++i)
    {
        const float cStep = static_cast<float>(i);
        bodies[i].Position = acRoot + glm::vec3{cStep * 9.3f - 40.f, std::sin(cStep) * 25.f, 18.f + cStep * 1.7f};
        bodies[i].Rotation = glm::normalize(glm::angleAxis(0.4f * cStep - 1.f, glm::normalize(glm::vec3{0.3f, cStep - 5.f, 1.f})));
    }
    return bodies;
}

float DegreesApart(const glm::quat& acA, const glm::quat& acB)
{
    return glm::degrees(2.f * std::acos(std::min(1.f, std::abs(glm::dot(acA, acB)))));
}

uint32_t LayoutOf(const std::vector<const char*>& acNames)
{
    Layout layout;
    for (const char* pName : acNames)
        layout.Add(pName);
    return layout.Key();
}

const std::vector<const char*> kWolfNames{"Canine_Pelvis", "Canine_Spine1", "Canine_Spine2", "Canine_Neck", "Canine_Head", "Canine_Tail1"};
} // namespace

TEST_CASE("a ragdoll pose goes over the wire unchanged", "[ragdoll_bodies_pose][encoding]")
{
    const glm::vec3 cRoot{-152340.25f, 98765.5f, -2400.75f};
    RagdollPose pose{};
    REQUIRE(Encode(0x1234'5678'9ABCull, 0xDEADBEEF, cRoot, Wolf(cRoot), pose));
    REQUIRE(pose.Bodies.size() == 12);

    SECTION("RequestRagdollPose")
    {
        RequestRagdollPose request{};
        request.ServerId = 0xFFFFFFFF;
        request.OwnershipEpoch = 0x1234;
        request.Pose = pose;
        const RequestRagdollPose cBack = ClientRoundTrip(request);
        CHECK(cBack == request);
        CHECK(cBack.Pose.Tick == 0x1234'5678'9ABCull);
        CHECK(cBack.Pose.Layout == 0xDEADBEEF);
        CHECK(cBack.Pose.BodyCount == 12);
        CHECK(cBack.Pose.Root[0] == cRoot.x);

        RequestRagdollPose empty{};
        CHECK(ClientRoundTrip(empty) == empty);
    }

    SECTION("NotifyRagdollPose")
    {
        NotifyRagdollPose notify{};
        notify.ServerId = 0x1A2B;
        notify.OwnershipEpoch = 0x5678;
        notify.Pose = pose;
        const NotifyRagdollPose cBack = ServerRoundTrip(notify);
        CHECK(cBack == notify);

        auto other = notify;
        other.Pose.Bodies[3].Offset[1] += 1;
        CHECK_FALSE(other == notify);
    }

    SECTION("A message with more bodies than it may carry, or than the ragdoll has, is dropped whole")
    {
        for (const uint8_t cBodyCount : {uint8_t{255}, uint8_t{4}})
        {
            Buffer buffer(1024);
            Buffer::Writer writer(&buffer);
            writer.WriteBits(kRequestRagdollPose, sizeof(ClientOpcode) * 8);
            Serialization::WriteVarInt(writer, 7); // server id
            Serialization::WriteVarInt(writer, 2); // ownership epoch
            Serialization::WriteVarInt(writer, 1); // tick
            writer.WriteBits(0, 32);               // layout
            writer.WriteBits(cBodyCount, 8);
            for (int axis = 0; axis < 3; ++axis)
                Serialization::WriteFloat(writer, 0.f);
            const uint32_t cSent = cBodyCount == 255 ? static_cast<uint32_t>(RagdollPose::kMaxBodies) + 1 : 5;
            writer.WriteBits(cSent, 6);
            for (uint32_t i = 0; i < cSent; ++i)
            {
                writer.WriteBits(0, 48);
                writer.WriteBits(0, 32);
            }
            Buffer::Reader reader(&buffer);
            const bool cDropped = !ClientMessageFactory{}.Extract(reader);
            CHECK(cDropped);
        }
    }
}

TEST_CASE("creature articulation wakes a stationary root", "[ragdoll_bodies_pose]")
{
    const auto cRest = Wolf({});
    auto moved = cRest;
    CHECK_FALSE(ArticulationChanged(cRest, moved));
    moved.back().Position.y += 0.5f;
    CHECK_FALSE(ArticulationChanged(cRest, moved));
    moved.back().Position.y += 5.f;
    CHECK(ArticulationChanged(cRest, moved));
    moved = cRest;
    moved.back().Rotation = glm::angleAxis(glm::radians(15.f), glm::vec3{0.f, 0.f, 1.f}) * moved.back().Rotation;
    CHECK(ArticulationChanged(cRest, moved));
    CHECK(moved.front().Position == cRest.front().Position);
}

TEST_CASE("a ragdoll's bodies are sent close to where they are", "[ragdoll_bodies_pose]")
{
    const glm::vec3 cRoot{30211.f, -41987.5f, 512.25f};
    const std::vector<Body> cBodies = Wolf(cRoot);
    RagdollPose pose{};
    REQUIRE(Encode(1, 2, cRoot, cBodies, pose));
    std::vector<Body> back;
    REQUIRE(Decode(pose, back));
    REQUIRE(back.size() == cBodies.size());
    for (size_t i = 0; i < back.size(); ++i)
    {
        CHECK(glm::distance(back[i].Position, cBodies[i].Position) <= RagdollPose::kOffsetStep);
        CHECK(DegreesApart(back[i].Rotation, cBodies[i].Rotation) < 0.5f);
    }

    SECTION("Only the first bodies go, and the count says how many it has in all")
    {
        RagdollPose dragon{};
        REQUIRE(Encode(1, 2, cRoot, Wolf(cRoot, 40), dragon));
        CHECK(dragon.Bodies.size() == RagdollPose::kMaxBodies);
        CHECK(dragon.BodyCount == 40);
    }

    SECTION("A body too far from the root, a reading that is not a number, or no bodies at all is not sent")
    {
        std::vector<Body> stray = cBodies;
        stray[5].Position.x = cRoot.x + 4200.f;
        CHECK_FALSE(Encode(1, 2, cRoot, stray, pose));

        std::vector<Body> broken = cBodies;
        broken[2].Rotation.w = std::numeric_limits<float>::quiet_NaN();
        CHECK_FALSE(Encode(1, 2, cRoot, broken, pose));
        broken = cBodies;
        broken[0].Position.y = std::numeric_limits<float>::infinity();
        CHECK_FALSE(Encode(1, 2, cRoot, broken, pose));

        CHECK_FALSE(Encode(1, 2, glm::vec3{std::numeric_limits<float>::quiet_NaN()}, cBodies, pose));
        CHECK_FALSE(Encode(1, 2, cRoot, {}, pose));
    }

    SECTION("A root that is not a number decodes to nothing")
    {
        pose.Root[1] = std::numeric_limits<float>::quiet_NaN();
        CHECK_FALSE(Decode(pose, back));
    }
}

TEST_CASE("a pose is used only on a ragdoll with the same bodies in the same order", "[ragdoll_bodies_pose]")
{
    const uint32_t cWolf = LayoutOf(kWolfNames);
    CHECK(LayoutOf(kWolfNames) == cWolf);

    // Two bodies found the other way round.
    std::vector<const char*> swapped = kWolfNames;
    std::swap(swapped[2], swapped[3]);
    CHECK(LayoutOf(swapped) != cWolf);

    // One more body, or one fewer.
    std::vector<const char*> more = kWolfNames;
    more.push_back("Canine_Tail2");
    CHECK(LayoutOf(more) != cWolf);
    std::vector<const char*> fewer(kWolfNames.begin(), kWolfNames.end() - 1);
    CHECK(LayoutOf(fewer) != cWolf);

    // Names run together differently are other bodies.
    CHECK(LayoutOf({"AB", "C"}) != LayoutOf({"A", "BC"}));
    // A node with no name still counts as a body.
    CHECK(LayoutOf({nullptr}) != LayoutOf({}));

    CHECK(Fits(6, cWolf, 6, 6, cWolf));
    CHECK(Fits(40, cWolf, 32, 40, cWolf));
    CHECK_FALSE(Fits(6, cWolf, 6, 7, cWolf));     // another count here
    CHECK_FALSE(Fits(6, cWolf, 6, 6, cWolf + 1)); // another order or names
    CHECK_FALSE(Fits(6, cWolf, 7, 6, cWolf));     // more sent than it has
    CHECK_FALSE(Fits(0, cWolf, 0, 0, cWolf));
}

TEST_CASE("a copy's bodies are shown between its owner's poses, and not once they stop", "[ragdoll_bodies_pose]")
{
    const auto cStart = Clock::time_point{} + 1h;
    const glm::vec3 cRoot{100.f, 200.f, 300.f};
    const uint32_t cLayout = LayoutOf(kWolfNames);
    const std::vector<Body> cFirst = Wolf(cRoot, 6);
    std::vector<Body> second = cFirst;
    for (Body& body : second)
        body.Position += glm::vec3{50.f, 0.f, 0.f};

    Track track;
    std::vector<Body> shown;
    CHECK_FALSE(track.Fresh(cStart));
    CHECK_FALSE(track.BodiesAt(cStart, 6, cLayout, shown));

    REQUIRE(track.Push(1000, cLayout, 6, cFirst, cStart));
    REQUIRE(track.Push(1100, cLayout, 6, second, cStart + 100ms));

    SECTION("Played back behind the newest, halfway between two poses at half the time between them")
    {
        // 60 ms after the newest arrived, the time shown is 1100 + 60 - 120 = 1040: 40 of the 100 ms between the two.
        REQUIRE(track.BodiesAt(cStart + 160ms, 6, cLayout, shown));
        REQUIRE(shown.size() == 6);
        CHECK(shown[0].Position.x == Approx(cFirst[0].Position.x + 50.f * 0.4f).margin(0.01f));

        // Long enough after: held at the newest, not run on past it.
        REQUIRE(track.BodiesAt(cStart + 100ms + std::chrono::milliseconds(kDelayMs) + 50ms, 6, cLayout, shown));
        CHECK(shown[0].Position.x == Approx(second[0].Position.x));
    }

    SECTION("A pose not newer than the newest is dropped")
    {
        CHECK_FALSE(track.Push(1050, cLayout, 6, cFirst, cStart + 120ms));
        CHECK_FALSE(track.Push(1100, cLayout, 6, cFirst, cStart + 120ms));
        REQUIRE(track.BodiesAt(cStart + 100ms + std::chrono::milliseconds(kDelayMs), 6, cLayout, shown));
        CHECK(shown[0].Position.x == Approx(second[0].Position.x));
    }

    SECTION("Stale once its owner stops sending")
    {
        CHECK(track.Fresh(cStart + 100ms + kStale));
        CHECK_FALSE(track.Fresh(cStart + 100ms + kStale + 1ms));
        CHECK_FALSE(track.BodiesAt(cStart + 100ms + kStale + 1ms, 6, cLayout, shown));
    }

    SECTION("Never on another ragdoll")
    {
        CHECK_FALSE(track.BodiesAt(cStart + 150ms, 7, cLayout, shown));
        CHECK_FALSE(track.BodiesAt(cStart + 150ms, 6, cLayout ^ 1, shown));
    }

    SECTION("Another skeleton, or a clock set back, starts over rather than blending")
    {
        std::vector<Body> other = Wolf(cRoot, 5);
        REQUIRE(track.Push(1200, cLayout ^ 1, 5, other, cStart + 200ms));
        REQUIRE(track.BodiesAt(cStart + 210ms, 5, cLayout ^ 1, shown));
        CHECK(shown[0].Position.x == Approx(other[0].Position.x));

        REQUIRE(track.Push(10, cLayout ^ 1, 5, other, cStart + 300ms));
        REQUIRE(track.BodiesAt(cStart + 310ms, 5, cLayout ^ 1, shown));
        CHECK(shown[0].Position.x == Approx(other[0].Position.x));
    }

    SECTION("Another owner, whose clock is a little behind the last one's, is taken once the last one's poses stopped")
    {
        const auto cHandedOver = cStart + 100ms + kStale + 1ms;
        REQUIRE(track.Push(1050, cLayout, 6, cFirst, cHandedOver));
        REQUIRE(track.BodiesAt(cHandedOver + 10ms, 6, cLayout, shown));
        CHECK(shown[0].Position.x == Approx(cFirst[0].Position.x));
        CHECK_FALSE(track.Push(1040, cLayout, 6, second, cHandedOver + 50ms));
    }
}

TEST_CASE("an owner's bodies are used only near where the copy's lie", "[ragdoll_bodies_pose]")
{
    const glm::vec3 cHere{-30000.f, 52000.f, -800.f};
    CHECK(Near(cHere, cHere));
    CHECK(Near(cHere + glm::vec3{0.f, 511.f, 0.f}, cHere));
    CHECK_FALSE(Near(cHere + glm::vec3{0.f, 0.f, 513.f}, cHere));
    CHECK_FALSE(Near(cHere + glm::vec3{9000.f, 0.f, 0.f}, cHere));
    CHECK_FALSE(Near(glm::vec3{std::numeric_limits<float>::quiet_NaN()}, cHere));
}

TEST_CASE("a creature's rest sent again while its copy already lies so is forgotten, not settled into", "[ragdoll_bodies_pose]")
{
    using CopyBodyRules::HeldRestPose;
    using CopyBodyRules::OnHeldRestPose;

    const auto cHeld = Wolf({30211.f, -41987.5f, 512.25f});
    // It settled while its owner's bodies came, then physics here twitched it a little.
    auto here = cHeld;
    here[3].Position.x += 1.5f;
    here[5].Rotation = glm::normalize(glm::angleAxis(glm::radians(4.f), glm::vec3{0.f, 1.f, 0.f}) * here[5].Rotation);
    REQUIRE(LiesIn(cHeld, here));
    CHECK(OnHeldRestPose(false, true, LiesIn(cHeld, here)) == HeldRestPose::Forget);

    SECTION("a body that lies apart is laid out")
    {
        here[7].Position.z -= 2.5f;
        CHECK_FALSE(LiesIn(cHeld, here));
        CHECK(OnHeldRestPose(false, true, LiesIn(cHeld, here)) == HeldRestPose::Settle);
    }

    SECTION("a body turned past the tolerance is laid out")
    {
        here[2].Rotation = glm::normalize(glm::angleAxis(glm::radians(8.f), glm::vec3{1.f, 0.f, 0.f}) * cHeld[2].Rotation);
        CHECK_FALSE(LiesIn(cHeld, here));
        CHECK(OnHeldRestPose(false, true, LiesIn(cHeld, here)) == HeldRestPose::Settle);
    }

    SECTION("only the bodies sent are compared")
    {
        const std::vector<Body> cSent(cHeld.begin(), cHeld.begin() + 6);
        here[10].Position.y += 40.f;
        CHECK(LiesIn(cSent, here));
        CHECK_FALSE(LiesIn(cHeld, here));
    }

    SECTION("more held than the ragdoll has, none held, or a reading that is no number: laid out")
    {
        CHECK_FALSE(LiesIn(cHeld, std::vector<Body>(here.begin(), here.begin() + 6)));
        CHECK_FALSE(LiesIn({}, here));
        here[0].Position.x = std::numeric_limits<float>::quiet_NaN();
        CHECK_FALSE(LiesIn(cHeld, here));
        here = cHeld;
        here[4].Rotation.w = std::numeric_limits<float>::quiet_NaN();
        CHECK_FALSE(LiesIn(cHeld, here));
    }
}

TEST_CASE("the server passes on an owner's ragdoll poses, two close together at most", "[ragdoll_bodies_pose][server]")
{
    using namespace RagdollPoseRelay;
    const Relayed cNone{};
    CHECK(ServerTakes(true, true, false, true, cNone, 1000));
    CHECK_FALSE(ServerTakes(false, true, false, true, cNone, 1000)); // no such actor
    CHECK_FALSE(ServerTakes(true, false, false, true, cNone, 1000)); // another game runs it
    CHECK_FALSE(ServerTakes(true, true, true, true, cNone, 1000));   // a player's character
    CHECK_FALSE(ServerTakes(true, true, false, false, cNone, 1000)); // no bodies

    // A late pose delivered with the next is passed on; a third within the burst is not.
    Relayed relayed = AfterRelay(cNone, 1000);
    CHECK(ServerTakes(true, true, false, true, relayed, 1010));
    relayed = AfterRelay(relayed, 1010);
    CHECK_FALSE(ServerTakes(true, true, false, true, relayed, 1020));
    CHECK(ServerTakes(true, true, false, true, relayed, 1000 + kBurstMs));

    // Ten a second always goes.
    relayed = {};
    for (uint64_t at = 5000; at < 7000; at += 100)
    {
        REQUIRE(ServerTakes(true, true, false, true, relayed, at));
        relayed = AfterRelay(relayed, at);
    }
}

TEST_CASE("a body's pose is sent about ten times a second", "[ragdoll_bodies_pose]")
{
    const auto cNow = Clock::time_point{} + 1h;
    CHECK(SendDue(Clock::time_point{}, cNow));
    CHECK_FALSE(SendDue(cNow, cNow + 50ms));
    CHECK(SendDue(cNow, cNow + kSendGap));
    // The snapshot that reads a body comes every 100 ms, a little early or late: each one is sent.
    CHECK(SendDue(cNow, cNow + 95ms));
}
