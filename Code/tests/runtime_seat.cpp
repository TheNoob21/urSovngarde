#include <catch2/catch.hpp>

#include <PlayerFurniture.h>
#include <TiltedCore/Serialization.hpp>
#include <glm/glm.hpp>
#include <Structs/RuntimeSeat.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/NotifyNpcFurniture.h>
#include <Messages/RequestNpcFurniture.h>

#include <limits>

// Furniture placed at run time is named by its base form and where it stands.

using namespace TiltedPhoques;

TEST_CASE("A run-time seat's place is sent in whole units", "[runtime_seat]")
{
    CHECK(RuntimeSeats::Round(12.4f) == 12);
    CHECK(RuntimeSeats::Round(12.6f) == 13);
    CHECK(RuntimeSeats::Round(-12.6f) == -13);
    CHECK(RuntimeSeats::Round(std::numeric_limits<float>::quiet_NaN()) == 0);
    CHECK(RuntimeSeats::Round(std::numeric_limits<float>::infinity()) == 0);
    CHECK(RuntimeSeats::Round(1e9f) == 0);
    const RuntimeSeat cSeat = RuntimeSeats::At(-150000.4f, 2048.5f, -7.f);
    CHECK(cSeat.Placed);
    CHECK(cSeat.X == -150000);
    CHECK(cSeat.Y == 2049);
    CHECK(cSeat.Z == -7);

    for (const int32_t cValue : {0, 1, -1, 123456, -123456, RuntimeSeats::kMaxCoordinate, -RuntimeSeats::kMaxCoordinate})
        CHECK(RuntimeSeats::Unzig(RuntimeSeats::Zig(cValue)) == cValue);
}

TEST_CASE("NPC seat words carry a run-time seat's place", "[runtime_seat]")
{
    TiltedPhoques::Vector<NpcFurnitureWord> words;
    words.push_back({0x1A2B, 0, 0x5D03, 1, 3, 4, RuntimeSeats::At(-150000.f, 2048.f, -7.f)});
    words.push_back({7, 2, 0x1A2B3, 1, 7, 1});
    words.push_back({0xFFFFFFFF, 0xFFFF, 0xFFFFFF, 0, 3, 0xFFFFFFFC, RuntimeSeat{true, RuntimeSeats::kMaxCoordinate, -RuntimeSeats::kMaxCoordinate, 0}});

    {
        Buffer buff(4096);
        RequestNpcFurniture sendMessage;
        sendMessage.Words = words;
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);
        Buffer::Reader reader(&buff);
        auto pMessage = ClientMessageFactory{}.Extract(reader);
        REQUIRE(pMessage);
        REQUIRE(*CastUnique<RequestNpcFurniture>(std::move(pMessage)) == sendMessage);
    }
    {
        Buffer buff(4096);
        NotifyNpcFurniture sendMessage;
        sendMessage.Words = words;
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);
        Buffer::Reader reader(&buff);
        auto pMessage = ServerMessageFactory{}.Extract(reader);
        REQUIRE(pMessage);
        REQUIRE(*CastUnique<NotifyNpcFurniture>(std::move(pMessage)) == sendMessage);
    }
    {
        // A coordinate no world has fails the whole message.
        Buffer buff(4096);
        NotifyNpcFurniture sendMessage;
        sendMessage.Words.push_back({7, 2, 0x1A2B3, 1, 7, 1, RuntimeSeat{true, std::numeric_limits<int32_t>::max(), 0, 0}});
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);
        Buffer::Reader reader(&buff);
        REQUIRE(!ServerMessageFactory{}.Extract(reader));
        REQUIRE(DecodeGuard::t_failed);
        DecodeGuard::t_failed = false;
    }
}

TEST_CASE("Player seat messages carry a run-time seat's place, and two such chairs are two seats", "[runtime_seat]")
{
    using namespace PlayerFurniture;
    const Seat cPlaced{{0, 0x5D03}, 0, RuntimeSeats::At(100.f, 200.f, 30.f)};
    const Seat cPlacedElsewhere{{0, 0x5D03}, 0, RuntimeSeats::At(400.f, 200.f, 30.f)};
    CHECK_FALSE(cPlaced == cPlacedElsewhere);

    RequestPlayerFurniture request;
    request.Request = {0x80000008, 3, 9, Action::Enter, cPlaced, 0};
    request.Cell = {5, 0xB4D2};
    {
        Buffer buffer(256);
        Buffer::Writer writer(&buffer);
        request.Serialize(writer);
        Buffer::Reader reader(&buffer);
        auto pRequest = ClientMessageFactory{}.Extract(reader);
        REQUIRE(pRequest);
        CHECK(*CastUnique<RequestPlayerFurniture>(std::move(pRequest)) == request);
    }

    NotifyPlayerFurniture notify;
    notify.Actor = request.Request.Actor;
    notify.Epoch = request.Request.Epoch;
    notify.Where = cPlaced;
    notify.Cell = request.Cell;
    notify.State = SeatState::Sitting;
    notify.Lease = 4;
    {
        Buffer buffer(256);
        Buffer::Writer writer(&buffer);
        notify.Serialize(writer);
        Buffer::Reader reader(&buffer);
        auto pNotify = ServerMessageFactory{}.Extract(reader);
        REQUIRE(pNotify);
        CHECK(*CastUnique<NotifyPlayerFurniture>(std::move(pNotify)) == notify);
    }

    // Two players on two run-time chairs of the same base, and on the same one.
    Claims claims;
    const Authority cFirst{0x10, 1, true, {5, 0xB4D2}};
    const Authority cSecond{0x20, 1, true, {5, 0xB4D2}};
    CHECK(claims.Apply({cFirst.Actor, cFirst.Epoch, 1, Action::Enter, cPlaced, 0}, cFirst, 100).Decision == Verdict::Granted);
    CHECK(claims.Apply({cSecond.Actor, cSecond.Epoch, 1, Action::Enter, cPlacedElsewhere, 0}, cSecond, 100).Decision == Verdict::Granted);
    const Authority cThird{0x30, 1, true, {5, 0xB4D2}};
    CHECK(claims.Apply({cThird.Actor, cThird.Epoch, 1, Action::Enter, cPlaced, 0}, cThird, 100).Decision == Verdict::Occupied);
}

TEST_CASE("A copy finds the furniture placed at run time here nearest the place its owner's game names", "[runtime_seat]")
{
    const RuntimeSeat cAt = RuntimeSeats::At(1000.f, -2000.f, 50.f);
    TiltedPhoques::Vector<NpcFurniture::RuntimeCandidate> candidates;
    CHECK(NpcFurniture::NearestRuntimeSeat(candidates, cAt) == 0);

    // One too far, one within reach, one nearer still.
    candidates.push_back({0xFF000010, 1000.f + NpcFurniture::kRuntimeSeatReach + 1.f, -2000.f, 50.f});
    CHECK(NpcFurniture::NearestRuntimeSeat(candidates, cAt) == 0);
    candidates.push_back({0xFF000011, 1030.f, -2000.f, 50.f});
    CHECK(NpcFurniture::NearestRuntimeSeat(candidates, cAt) == 0xFF000011);
    candidates.push_back({0xFF000012, 1000.4f, -1999.6f, 50.f});
    CHECK(NpcFurniture::NearestRuntimeSeat(candidates, cAt) == 0xFF000012);

    // Within reach counts the height too; a seat not placed at run time, or no form, is never one.
    CHECK(NpcFurniture::RuntimeSeatFits({0xFF000013, 1000.f, -2000.f, 50.f + NpcFurniture::kRuntimeSeatReach}, cAt));
    CHECK_FALSE(NpcFurniture::RuntimeSeatFits({0xFF000013, 1000.f, -2000.f, 51.f + NpcFurniture::kRuntimeSeatReach}, cAt));
    CHECK_FALSE(NpcFurniture::RuntimeSeatFits({0xFF000013, 1000.f, -2000.f, 50.f}, RuntimeSeat{}));
    CHECK_FALSE(NpcFurniture::RuntimeSeatFits({0, 1000.f, -2000.f, 50.f}, cAt));
    CHECK_FALSE(NpcFurniture::RuntimeSeatFits({0xFF000013, std::numeric_limits<float>::quiet_NaN(), -2000.f, 50.f}, cAt));
}

TEST_CASE("An NPC in furniture placed at run time is seated in its base form at that place", "[runtime_seat]")
{
    const RuntimeSeat cAt = RuntimeSeats::At(10.f, 20.f, 30.f);
    const NpcFurniture::Seat cSeated = NpcFurniture::ReadSeat(NpcFurniture::kStateSitting, true, true, false, true, 0, 0x5D03, 1);
    const NpcFurniture::Seat cPlaced = NpcFurniture::WithPlacement(cSeated, cAt);
    CHECK(cPlaced.FurnitureBaseId == 0x5D03);
    CHECK(cPlaced.Placement == cAt);
    CHECK_FALSE(cPlaced == cSeated);
    // Not seated stays nothing at all, so its "got up" word is the same as any other's.
    const NpcFurniture::Seat cNone = NpcFurniture::WithPlacement(NpcFurniture::ReadSeat(NpcFurniture::kStateNone, true, true, false, true, 0, 0x5D03, 1), cAt);
    CHECK(cNone == NpcFurniture::Seat{});
}

#include <ClientTrust.h>

static_assert(RuntimeSeats::kWorldCoordinate == static_cast<int32_t>(ClientTrust::kMaxCoordinate));

TEST_CASE("A run-time seat beyond the world's range, or two chairs that cannot be told apart, seat nobody", "[runtime_seat]")
{
    CHECK(RuntimeSeats::InWorld(RuntimeSeat{true, RuntimeSeats::kWorldCoordinate, -RuntimeSeats::kWorldCoordinate, 0}));
    CHECK_FALSE(RuntimeSeats::InWorld(RuntimeSeat{true, RuntimeSeats::kWorldCoordinate + 1, 0, 0}));
    CHECK_FALSE(RuntimeSeats::InWorld(RuntimeSeat{}));

    // Beyond the world: nothing is matched there, even a candidate at that very place.
    const RuntimeSeat cFar{true, 0, 0, RuntimeSeats::kWorldCoordinate + 10};
    TiltedPhoques::Vector<NpcFurniture::RuntimeCandidate> far;
    far.push_back({0xFF000020, 0.f, 0.f, static_cast<float>(RuntimeSeats::kWorldCoordinate + 10)});
    CHECK(NpcFurniture::NearestRuntimeSeat(far, cFar) == 0);

    // Two of that base within reach: the nearer is taken when it is clearly nearer, none when both are about as near.
    const RuntimeSeat cAt = RuntimeSeats::At(0.f, 0.f, 0.f);
    TiltedPhoques::Vector<NpcFurniture::RuntimeCandidate> two;
    two.push_back({0xFF000021, 10.f, 0.f, 0.f});
    two.push_back({0xFF000022, -30.f, 0.f, 0.f});
    CHECK(NpcFurniture::NearestRuntimeSeat(two, cAt) == 0xFF000021);
    two.push_back({0xFF000023, 0.f, 10.5f, 0.f});
    CHECK(NpcFurniture::NearestRuntimeSeat(two, cAt) == 0);
    // Stacked at one spot (placed twice): none either.
    TiltedPhoques::Vector<NpcFurniture::RuntimeCandidate> stacked;
    stacked.push_back({0xFF000024, 3.f, 0.f, 0.f});
    stacked.push_back({0xFF000025, 3.f, 0.f, 0.f});
    CHECK(NpcFurniture::NearestRuntimeSeat(stacked, cAt) == 0);
}

TEST_CASE("Players' run-time chairs a few units apart are one chair to sit in", "[runtime_seat]")
{
    using namespace PlayerFurniture;
    const Seat cHere{{0, 0x5D03}, 0, RuntimeSeats::At(100.f, 200.f, 30.f)};
    const Seat cNearlyHere{{0, 0x5D03}, 0, RuntimeSeats::At(108.f, 205.f, 30.f)};
    const Seat cNextChair{{0, 0x5D03}, 0, RuntimeSeats::At(150.f, 200.f, 30.f)};
    const Seat cOtherMarker{{0, 0x5D03}, 1, RuntimeSeats::At(100.f, 200.f, 30.f)};
    const Seat cShared{{0, 0x5D03}, 0};

    // Exactly equal only at the same place; one chair within kSameChairUnits.
    CHECK_FALSE(cHere == cNearlyHere);
    CHECK(SameChair(cHere, cHere));
    CHECK(SameChair(cHere, cNearlyHere));
    CHECK_FALSE(SameChair(cHere, cNextChair));
    CHECK_FALSE(SameChair(cHere, cOtherMarker));
    CHECK_FALSE(SameChair(cHere, cShared));
    CHECK(SameChair(cShared, cShared));

    Claims claims;
    const Authority cFirst{0x10, 1, true, {5, 0xB4D2}};
    const Authority cSecond{0x20, 1, true, {5, 0xB4D2}};
    const Authority cThird{0x30, 1, true, {5, 0xB4D2}};
    CHECK(claims.Apply({cFirst.Actor, cFirst.Epoch, 1, Action::Enter, cHere, 0}, cFirst, 100).Decision == Verdict::Granted);
    CHECK(claims.Apply({cSecond.Actor, cSecond.Epoch, 1, Action::Enter, cNearlyHere, 0}, cSecond, 100).Decision == Verdict::Occupied);
    CHECK(claims.Apply({cThird.Actor, cThird.Epoch, 1, Action::Enter, cNextChair, 0}, cThird, 100).Decision == Verdict::Granted);
}
