#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <catch2/catch.hpp>

#include <DecodeGuard.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/RequestPropBodies.h>
#include <Messages/NotifyPropBodies.h>

#include <Services/WorldObjectBodies.h>
#include <Games/Skyrim/RagdollBodiesPose.h>
#include <ObjectMover.h>
#include <PropBodyRelay.h>

// A placed object's hinged and hanging bodies (WorldObjectBodies.h): the Riverwood Trader sign's board, rings and charms swing on
// hinges below a fixed root, so the swing is sent as the bodies' poses in the root's space, and driven with a speed elsewhere.

using namespace TiltedPhoques;
using namespace WorldObjectBodies;
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

glm::quat Turn(const float aAngle, const glm::vec3& acAxis)
{
    return glm::angleAxis(aAngle, glm::normalize(acAxis));
}

//! The Riverwood Trader sign at rest: seven bodies hanging below the bracket (SignRiverwoodRiverwoodTrader01.nif).
Poses Sign()
{
    Poses poses;
    const float cDrops[] = {-16.7f, -24.6f, -32.5f, -50.6f, -77.7f, -84.f, -84.f};
    for (size_t i = 0; i < 7; ++i)
        poses.push_back({glm::vec3{1.f, 77.4f + (i == 5 ? -19.6f : i == 6 ? 19.6f : 0.f), cDrops[i]}, glm::quat{1.f, 0.f, 0.f, 0.f}});
    return poses;
}

//! The sign swung by aAngle about its hinge axis (x), each body turned about the bracket.
Poses Swung(const float aAngle)
{
    Poses poses = Sign();
    const glm::vec3 cPivot{1.f, 77.4f, 0.f};
    const glm::quat cTurn = Turn(aAngle, {1.f, 0.f, 0.f});
    for (Pose& pose : poses)
    {
        pose.Position = cPivot + cTurn * (pose.Position - cPivot);
        pose.Rotation = cTurn * pose.Rotation;
    }
    return poses;
}
} // namespace

TEST_CASE("Prop bodies: both messages keep the object, its place and its bodies", "[prop_bodies][encoding]")
{
    RequestPropBodies request{};
    request.ObjectId = GameId(0, 0x3A2C1);
    request.CellId = GameId(0, 0x9732);
    request.WorldSpaceId = GameId(0, 0x3C);
    request.Position = glm::vec3(17305.f, -48021.f, -3410.f);
    request.Bodies.Layout = 0xcc1234afu;
    for (const Pose& pose : Swung(0.6f))
        request.Bodies.Bodies.push_back(pose);
    request.Bodies.Bodies[2].Position = {-1999.5f, 0.0625f, -0.03f};
    request.AtRest = false;
    request.Tick = 0x1234'5678'9ABCull;

    const RequestPropBodies cRequestBack = ClientRoundTrip(request);
    REQUIRE(cRequestBack == request);
    REQUIRE(cRequestBack.Bodies.Bodies.size() == 7);
    CHECK(cRequestBack.Bodies.Layout == request.Bodies.Layout);
    for (size_t i = 0; i < 7; ++i)
    {
        // Sixteenths of a unit, and the network quaternion's 10 bits a component.
        CHECK(glm::distance(cRequestBack.Bodies.Bodies[i].Position, request.Bodies.Bodies[i].Position) <= 0.06f);
        CHECK(AngleBetween(cRequestBack.Bodies.Bodies[i].Rotation, request.Bodies.Bodies[i].Rotation) < 0.01f);
    }

    NotifyPropBodies notify{};
    notify.ObjectId = request.ObjectId;
    notify.CellId = request.CellId;
    notify.WorldSpaceId = request.WorldSpaceId;
    notify.Position = request.Position;
    notify.Bodies = cRequestBack.Bodies;
    notify.AtRest = true;
    notify.Tick = request.Tick;
    const NotifyPropBodies cNotifyBack = ServerRoundTrip(notify);
    REQUIRE(cNotifyBack == notify);
    CHECK(cNotifyBack.AtRest);
    CHECK(cNotifyBack.Bodies.Layout == request.Bodies.Layout);
    CHECK(cNotifyBack.Tick == request.Tick);
}

TEST_CASE("Prop bodies: no more bodies than there is room for travel", "[prop_bodies][encoding]")
{
    SECTION("a sender with more writes the first sixteen")
    {
        PropBodyState state{};
        state.Bodies.resize(PropBodyState::kMaxBodies + 4);
        Buffer buffer(1024);
        Buffer::Writer writer(&buffer);
        state.Serialize(writer);
        Buffer::Reader reader(&buffer);
        PropBodyState back{};
        back.Deserialize(reader);
        CHECK(back.Bodies.size() == PropBodyState::kMaxBodies);
    }

    SECTION("a count past that fails the message")
    {
        Buffer buffer(1024);
        Buffer::Writer writer(&buffer);
        writer.WriteBits(1, 32);
        Serialization::WriteVarInt(writer, PropBodyState::kMaxBodies + 1);
        Buffer::Reader reader(&buffer);
        DecodeGuard::t_failed = false;
        PropBodyState back{};
        back.Deserialize(reader);
        CHECK(back.Bodies.empty());
        CHECK(DecodeGuard::t_failed);
        DecodeGuard::t_failed = false;
    }

    SECTION("the factory drops a message with a count past that")
    {
        Buffer buffer(1024);
        Buffer::Writer writer(&buffer);
        writer.WriteBits(RequestPropBodies::Opcode, sizeof(ClientOpcode) * 8);
        GameId(0, 0x3A2C1).Serialize(writer);
        GameId(0, 0x9732).Serialize(writer);
        GameId(0, 0x3C).Serialize(writer);
        Vector3_NetQuantize place{};
        place = glm::vec3(17305.f, -48021.f, -3410.f);
        place.Serialize(writer);
        writer.WriteBits(1, 32);
        Serialization::WriteVarInt(writer, PropBodyState::kMaxBodies + 1);
        Buffer::Reader reader(&buffer);
        CHECK_FALSE(ClientMessageFactory{}.Extract(reader));
        DecodeGuard::t_failed = false;
    }
}

TEST_CASE("Prop bodies: equal counts with permuted hinges do not fit", "[prop_bodies]")
{
    RagdollBodiesPose::Layout remote;
    remote.Add("Board");
    remote.Add("Ring");
    RagdollBodiesPose::Layout local;
    local.Add("Ring");
    local.Add("Board");
    CHECK(RagdollBodiesPose::Fits(2, remote.Key(), 2, 2, remote.Key()));
    CHECK_FALSE(RagdollBodiesPose::Fits(2, remote.Key(), 2, 2, local.Key()));
}

TEST_CASE("Prop bodies: the relay bounds pose traffic and keeps one final rest", "[prop_bodies]")
{
    const auto cNow = Clock::time_point{} + 10s;
    const GameId cCell{0, 100};
    const GameId cOtherCell{0, 101};
    const GameId cInterior{};
    PropBodyRelay::Rate rate{};
    PropBodyRelay relay{};
    CHECK_FALSE(relay.Accept(1, cCell, cInterior, true, 99, cNow, rate));
    REQUIRE(relay.Accept(1, cCell, cInterior, false, 100, cNow, rate));
    CHECK_FALSE(relay.Accept(1, cCell, cInterior, false, 101, cNow + 10ms, rate));
    CHECK_FALSE(relay.Accept(1, cCell, cInterior, false, 100, cNow + 100ms, rate));
    CHECK_FALSE(relay.Accept(2, cCell, cInterior, false, 101, cNow + 100ms, rate));
    CHECK_FALSE(relay.Accept(1, cOtherCell, cInterior, false, 101, cNow + 100ms, rate));
    for (uint32_t i = 1; i < PropBodyRelay::kUpdatesPerSecond; ++i)
        REQUIRE(rate.Allow(cNow));
    CHECK_FALSE(relay.Accept(1, cCell, cInterior, false, 101, cNow + 100ms, rate));
    CHECK(relay.Accept(1, cCell, cInterior, true, 102, cNow + 101ms, rate));
    CHECK_FALSE(relay.Accept(1, cCell, cInterior, true, 103, cNow + 102ms, rate));
    CHECK(relay.Accept(2, cCell, cInterior, false, 1, cNow + 1s, rate));
}

TEST_CASE("Prop bodies: a pose sent before the rest it overtook is not passed on after the rest lapses", "[prop_bodies]")
{
    const auto cNow = Clock::time_point{} + 10s;
    const GameId cCell{0, 100};
    const GameId cInterior{};
    PropBodyRelay::Rate rate{};
    PropBodyRelay relay{};
    REQUIRE(relay.Accept(1, cCell, cInterior, false, 100, cNow, rate));
    REQUIRE(relay.Accept(1, cCell, cInterior, true, 300, cNow + 200ms, rate));
    CHECK_FALSE(relay.Accept(1, cCell, cInterior, false, 200, cNow + 200ms + PropBodyRelay::Clock::duration(ObjectMover::kHeldForAtRest) + 1ms, rate));
    CHECK(relay.Accept(1, cCell, cInterior, false, 400, cNow + 2s, rate));
}

TEST_CASE("Prop bodies: a swing is seen from the bodies while the root stays put", "[prop_bodies]")
{
    const auto cAt = Clock::time_point{} + 10s;
    Track track{};
    track.Settle(Sign());
    float distance = 0.f;
    float angle = 0.f;

    SECTION("settling jitter is not a swing")
    {
        Poses jitter = Sign();
        jitter[3].Position.z += 1.f;
        jitter[6].Rotation = Turn(0.05f, {0.f, 1.f, 0.f});
        CHECK(track.Next(jitter, cAt, distance, angle) == Step::None);
    }

    SECTION("one charm swung is a start, though nothing else moved")
    {
        Poses charm = Sign();
        charm[5].Position.y += 6.f;
        CHECK(track.Next(charm, cAt, distance, angle) == Step::Start);
        CHECK(distance == Approx(6.f));
    }

    SECTION("a turn alone is a start")
    {
        Poses turned = Sign();
        turned[2].Rotation = Turn(0.2f, {1.f, 0.f, 0.f});
        CHECK(track.Next(turned, cAt, distance, angle) == Step::Start);
        CHECK(angle == Approx(0.2f).margin(1e-3));
    }

    SECTION("another count of bodies starts over from it")
    {
        Poses fewer = Sign();
        fewer.pop_back();
        CHECK(track.Next(fewer, cAt, distance, angle) == Step::None);
        CHECK(track.Rested.size() == 6);
        CHECK(track.Next(fewer, cAt, distance, angle) == Step::None);
    }
}

TEST_CASE("Prop bodies: sent ten times a second while they swing, then once at rest", "[prop_bodies]")
{
    const auto cStart = Clock::time_point{} + 10s;
    Track track{};
    track.Settle(Sign());
    track.Begin(Swung(0.3f), cStart);
    float distance = 0.f;
    float angle = 0.f;

    // Moving, but a send was made 33 ms ago. The 33 ms pass grid makes the 99 ms pass the third.
    CHECK(track.Next(Swung(0.25f), cStart + 33ms, distance, angle) == Step::None);
    CHECK(track.Next(Swung(0.2f), cStart + 99ms, distance, angle) == Step::Move);
    CHECK(track.Sent.size() == 7);
    // A swing's top: still for less than a second is not at rest.
    CHECK(track.Next(Swung(0.2f), cStart + 600ms, distance, angle) == Step::None);
    CHECK(track.Next(Swung(-0.1f), cStart + 700ms, distance, angle) == Step::Move);
    CHECK(track.Next(Swung(-0.1f), cStart + 1500ms, distance, angle) == Step::None);
    CHECK(track.Moving);
    CHECK(track.Next(Swung(-0.1f), cStart + 1700ms, distance, angle) == Step::Rest);
    CHECK_FALSE(track.Moving);
    // A restart is measured from where they came to rest.
    CHECK(track.Next(Swung(-0.1f), cStart + 1800ms, distance, angle) == Step::None);
    CHECK(track.Next(Swung(0.1f), cStart + 1900ms, distance, angle) == Step::Start);
}

TEST_CASE("Prop bodies: poses in the root's space come back to the same world pose", "[prop_bodies]")
{
    const glm::mat3 cRootTurn = glm::mat3_cast(Turn(2.1f, {0.2f, -0.3f, 1.f}));
    const glm::vec3 cRootAt{17305.f, -48021.f, -3410.f};
    const float cRootScale = 1.3f;
    const glm::vec3 cWorldAt{17290.f, -47950.f, -3480.f};
    const glm::quat cWorldTurn = Turn(-0.7f, {1.f, 1.f, 0.f});

    const Pose cLocal = ToRoot(cRootTurn, cRootAt, cRootScale, cWorldAt, cWorldTurn);
    const Pose cBack = FromRoot(cRootTurn, cRootAt, cRootScale, cLocal);
    CHECK(glm::distance(cBack.Position, cWorldAt) < 1e-2f);
    CHECK(AngleBetween(cBack.Rotation, cWorldTurn) < 3e-3f);
    // The root's scale is taken out.
    CHECK(glm::length(cLocal.Position) == Approx(glm::distance(cWorldAt, cRootAt) / cRootScale).epsilon(1e-4));
}

TEST_CASE("Prop bodies: a driven body is given the speed that reaches its target", "[prop_bodies]")
{
    SECTION("moved there in kReachSeconds, capped")
    {
        const glm::vec3 cFrom{10.f, 20.f, 30.f};
        const glm::vec3 cTo{14.f, 17.f, 30.f};
        const glm::vec3 cVelocity = LinearToward(cFrom, cTo);
        CHECK(glm::distance(cFrom + cVelocity * kReachSeconds, cTo) < 1e-4f);
        CHECK(glm::length(LinearToward(cFrom, cFrom + glm::vec3{5000.f, 0.f, 0.f})) == Approx(kMaxSpeed));
        CHECK(glm::length(LinearToward(cFrom, cFrom)) == 0.f);
    }

    SECTION("turned there in kReachSeconds, the short way")
    {
        const glm::quat cFrom = Turn(0.3f, {0.f, 0.f, 1.f});
        const glm::quat cTo = Turn(0.8f, {0.f, 0.f, 1.f});
        const glm::vec3 cSpin = AngularToward(cFrom, cTo);
        CHECK(cSpin.z == Approx(0.5f / kReachSeconds));
        CHECK(std::fabs(cSpin.x) < 1e-5f);
        // The same rotation written negated turns no differently.
        const glm::vec3 cNegated = AngularToward(cFrom, -cTo);
        CHECK(glm::distance(cSpin, cNegated) < 1e-3f);
        // Turned by that spin for kReachSeconds, it is at the target.
        const float cRadians = glm::length(cSpin) * kReachSeconds;
        const glm::quat cReached = Turn(cRadians, cSpin) * cFrom;
        CHECK(AngleBetween(cReached, cTo) < 3e-3f);
        CHECK(glm::length(AngularToward(cFrom, cFrom)) == 0.f);
        CHECK(glm::length(AngularToward(Turn(0.f, {1.f, 0.f, 0.f}), Turn(3.1f, {1.f, 0.f, 0.f}))) == Approx(kMaxSpin));
    }
}

TEST_CASE("Prop bodies: another player's swing is let go once it rests there or stops coming", "[prop_bodies]")
{
    const auto cAt = Clock::time_point{} + 10s;
    Driven driven{};
    CHECK_FALSE(driven.LetGo(cAt + 1h));

    CHECK(driven.Take(Swung(0.4f), false, 1000, cAt));
    CHECK(driven.On);
    CHECK_FALSE(driven.LetGo(cAt + 1900ms));
    CHECK(driven.LetGo(cAt + 2100ms));

    CHECK(driven.Take(Sign(), true, 1100, cAt));
    CHECK_FALSE(driven.LetGo(cAt + 400ms));
    CHECK(driven.LetGo(cAt + 600ms));
}

TEST_CASE("Prop bodies: a driven target moves on at the sender's speed, timed by its ticks", "[prop_bodies]")
{
    const auto cAt = Clock::time_point{} + 10s;
    Driven driven{};
    REQUIRE(driven.Take(Swung(0.2f), false, 5000, cAt));
    // The first update has no speed yet.
    CHECK(glm::length(driven.Target[2].Velocity) == 0.f);

    // 100 ms later by the sender's clock, though it arrived 160 ms later here: its speed is over the sender's 100 ms.
    REQUIRE(driven.Take(Swung(0.3f), false, 5100, cAt + 160ms));
    const glm::vec3 cMoved = Swung(0.3f)[4].Position - Swung(0.2f)[4].Position;
    CHECK(glm::distance(driven.Target[4].Velocity, cMoved / 0.1f) < 1e-2f);
    CHECK(glm::length(driven.Target[4].Spin) == Approx(1.f).margin(1e-2));

    SECTION("carried on between updates, at most kMaxLead")
    {
        const Targets cSoon = driven.Now(cAt + 160ms + 50ms);
        CHECK(glm::distance(cSoon[4].At.Position, Swung(0.3f)[4].Position + cMoved / 0.1f * 0.05f) < 1e-2f);
        CHECK(AngleBetween(cSoon[4].At.Rotation, Swung(0.35f)[4].Rotation) < 3e-3f);
        const Targets cLate = driven.Now(cAt + 160ms + 2s);
        CHECK(AngleBetween(cLate[4].At.Rotation, Swung(0.3f + kMaxLead)[4].Rotation) < 3e-3f);
    }

    SECTION("an older update is refused, an equal one too")
    {
        CHECK_FALSE(driven.Take(Swung(0.9f), false, 5050, cAt + 200ms));
        CHECK_FALSE(driven.Take(Swung(0.9f), false, 5100, cAt + 200ms));
        CHECK(AngleBetween(driven.Target[0].At.Rotation, Swung(0.3f)[0].Rotation) < 3e-3f);
    }

    SECTION("a rest stands still")
    {
        REQUIRE(driven.Take(Swung(0.32f), true, 5200, cAt + 260ms));
        CHECK(glm::length(driven.Target[4].Velocity) == 0.f);
        CHECK(glm::length(driven.Target[4].Spin) == 0.f);
    }

    SECTION("a gap over a second gives no speed")
    {
        REQUIRE(driven.Take(Swung(0.5f), false, 7000, cAt + 2s));
        CHECK(glm::length(driven.Target[4].Velocity) == 0.f);
    }
}

TEST_CASE("Prop bodies: another player's swing is not sent back", "[prop_bodies]")
{
    const auto cAt = Clock::time_point{} + 10s;
    float distance = 0.f;
    float angle = 0.f;
    Track track{};
    track.Settle(Sign());

    SECTION("while driven, held or quiet the bodies are only settled")
    {
        CHECK(track.Observe(Swung(0.5f), true, cAt, distance, angle) == Step::None);
        CHECK(track.Observe(Swung(-0.5f), true, cAt + 33ms, distance, angle) == Step::None);
        CHECK(track.Rested.size() == 7);
    }

    SECTION("the tail of their swing after the let-go is held until still")
    {
        Driven driven{};
        REQUIRE(driven.Take(Sign(), true, 1000, cAt - 1s));
        REQUIRE(driven.Release(track, cAt));
        CHECK_FALSE(driven.On);
        CHECK_FALSE(driven.Release(track, cAt + 2s));
        // Still swinging here once let go: none of it is a start.
        for (int pass = 0; pass < 30; ++pass)
            CHECK(track.Observe(Swung(pass % 2 ? 0.2f : -0.2f), false, cAt + pass * 33ms, distance, angle) == Step::None);
        // Then still for a second: the hold ends, and only a new swing is a start.
        const auto cStill = cAt + 30 * 33ms;
        for (int pass = 0; pass <= 31; ++pass)
            CHECK(track.Observe(Sign(), false, cStill + pass * 33ms, distance, angle) == Step::None);
        CHECK(track.Observe(Swung(0.4f), false, cStill + 32 * 33ms, distance, angle) == Step::Start);
    }

    SECTION("a hold ends at its cap even if the bodies never settle")
    {
        track.Hold(cAt);
        CHECK(track.Observe(Swung(0.2f), false, cAt + 4s, distance, angle) == Step::None);
        CHECK(track.Observe(Swung(0.6f), false, cAt + kHoldCap + 1ms, distance, angle) == Step::Start);
    }

    SECTION("this game's own swing yields to theirs")
    {
        track.Begin(Swung(0.3f), cAt);
        track.GiveWay();
        CHECK_FALSE(track.Moving);
        CHECK(track.Observe(Swung(0.1f), true, cAt + 33ms, distance, angle) == Step::None);
    }

    SECTION("a swing this game was sending, cut short, ends with a rest")
    {
        track.Begin(Swung(0.3f), cAt);
        CHECK(track.Observe(Swung(0.25f), true, cAt + 33ms, distance, angle) == Step::Rest);
        CHECK_FALSE(track.Moving);
    }
}

TEST_CASE("Prop bodies: the server forgets a mover once it lapses", "[prop_bodies]")
{
    using MoverClock = ObjectMover::Clock;
    const auto cAt = MoverClock::time_point{} + 10s;
    ObjectMover mover{};
    CHECK(mover.IsLapsed(cAt));
    REQUIRE(mover.Take(1, false, cAt));
    CHECK_FALSE(mover.IsLapsed(cAt + 1s));
    CHECK(mover.IsLapsed(cAt + ObjectMover::kHeldFor));
    REQUIRE(mover.Take(1, true, cAt + 3s));
    CHECK(mover.IsLapsed(cAt + 3s + ObjectMover::kHeldForAtRest));
}

TEST_CASE("Prop bodies: which bodies swing and which objects hang them", "[prop_bodies]")
{
    // hkpMotion::MotionType: the sign's bracket is fixed (5) and its board box inertia (3), as the mesh stores them.
    CHECK(IsDynamicMotion(1));
    CHECK(IsDynamicMotion(2));
    CHECK(IsDynamicMotion(3));
    CHECK(IsDynamicMotion(6));
    CHECK_FALSE(IsDynamicMotion(0));
    CHECK_FALSE(IsDynamicMotion(4));
    CHECK_FALSE(IsDynamicMotion(5));
    CHECK_FALSE(IsDynamicMotion(7));

    // Movable statics (Riverwood's signs), activators (Windhelm's), containers, flora, furniture; never items or statics.
    for (const int cType : {24, 28, 36, 39, 40})
        CHECK(IsBodiesType(static_cast<uint8_t>(cType)));
    for (const int cType : {26, 32, 33, 34, 41, 43})
        CHECK_FALSE(IsBodiesType(static_cast<uint8_t>(cType)));
}

TEST_CASE("Prop bodies: a sample is read again only once it would go stale before the next frame", "[prop_bodies]")
{
    using namespace std::chrono_literals;
    const WorldObjectBodies::Clock::time_point cRead{};
    const auto cPoseLifetime = std::chrono::duration_cast<WorldObjectBodies::Clock::duration>(100ms);
    const auto cFrame = std::chrono::duration_cast<WorldObjectBodies::Clock::duration>(16ms);

    // Just read, it serves the next frames.
    REQUIRE(WorldObjectBodies::SampleServesNextFrame(cRead, cPoseLifetime, cRead, cFrame));
    REQUIRE(WorldObjectBodies::SampleServesNextFrame(cRead, cPoseLifetime, cRead + 60ms, cFrame));
    // Two frames short of its end it is read again, so it is never stale between frames.
    REQUIRE_FALSE(WorldObjectBodies::SampleServesNextFrame(cRead, cPoseLifetime, cRead + 68ms, cFrame));
    REQUIRE_FALSE(WorldObjectBodies::SampleServesNextFrame(cRead, cPoseLifetime, cRead + 100ms, cFrame));
    // Slow frames read it more often.
    REQUIRE_FALSE(WorldObjectBodies::SampleServesNextFrame(cRead, cPoseLifetime, cRead + 20ms, 50ms));
    // A probe, good for two seconds, waits much longer.
    REQUIRE(WorldObjectBodies::SampleServesNextFrame(cRead, 2s, cRead + 1900ms, cFrame));
}

TEST_CASE("Prop bodies: a long frame after short ones neither loses the poses nor sends a rest", "[prop_bodies]")
{
    constexpr uint32_t cLayout = 7;
    // As RunQueued: on each frame a sample not kept is read again; the update reads it at any moment between frames.
    // The long frame follows each number of short ones, so every age the sample can have when it starts is tried.
    const auto cHitch = [&](const Clock::duration aShort, const Clock::duration aLong, const int aShortFrames, bool& aAlwaysFresh, bool& aEnded)
    {
        Clock::time_point now = Clock::time_point{} + 10s;
        Clock::time_point readAt = now;
        for (int frame = 0; frame <= aShortFrames + 3; ++frame)
        {
            const Clock::duration cFrame = frame == aShortFrames ? aLong : aShort;
            for (auto at = now; at < now + cFrame; at += 1ms)
            {
                const bool cFresh = at - readAt < kPoseSampleFor;
                aAlwaysFresh = aAlwaysFresh && cFresh;
                aEnded = aEnded || EndsSwing(cFresh ? Sample::Read : Sample::Waiting, cLayout, cLayout);
            }
            now += cFrame;
            if (!KeepsSample(readAt, false, now, cFrame))
                readAt = now;
        }
    };

    SECTION("16 ms frames, then one of 60 ms")
    {
        for (int shortFrames = 1; shortFrames <= 12; ++shortFrames)
        {
            bool alwaysFresh = true;
            bool ended = false;
            cHitch(16ms, 60ms, shortFrames, alwaysFresh, ended);
            CHECK(alwaysFresh);
            CHECK_FALSE(ended);
        }
    }

    SECTION("a frame longer than a pose sample lives finds none, which ends nothing")
    {
        bool alwaysFresh = true;
        bool ended = false;
        cHitch(16ms, 150ms, 4, alwaysFresh, ended);
        CHECK_FALSE(alwaysFresh);
        CHECK_FALSE(ended);
    }

    SECTION("only an empty read or another layout ends a swing")
    {
        CHECK_FALSE(EndsSwing(Sample::Waiting, 0, cLayout));
        CHECK_FALSE(EndsSwing(Sample::Read, cLayout, cLayout));
        CHECK(EndsSwing(Sample::Empty, 0, cLayout));
        CHECK(EndsSwing(Sample::Read, cLayout + 1, cLayout));
    }

    SECTION("poses are read again about every pass, probes kept for most of their life")
    {
        const Clock::time_point cRead = Clock::time_point{} + 10s;
        CHECK(KeepsSample(cRead, false, cRead + 16ms, 16ms));
        CHECK_FALSE(KeepsSample(cRead, false, cRead + kPoseReuseFor, 1ms));
        CHECK(KeepsSample(cRead, true, cRead + 1500ms, 16ms));
        CHECK_FALSE(KeepsSample(cRead, true, cRead + 1990ms, 16ms));
    }
}
