#include <catch2/catch.hpp>

#include <PlayerFurniture.h>
#include <TiltedCore/Serialization.hpp>
#include <glm/glm.hpp>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>

using namespace PlayerFurniture;

namespace
{
const Seat kChair{{0, 0x1A2B3}, 0};
const Seat kOtherMarker{kChair.Reference, 1};
const Seat kBed{{2, 0x2C3D4}, 0};

Request Enter(const Authority& acActor, uint64_t aSequence, const Seat& acSeat) noexcept
{
    return {acActor.Actor, acActor.Epoch, aSequence, Action::Enter, acSeat, 0};
}

Request Change(const Claim& acClaim, uint64_t aSequence, Action aAction) noexcept
{
    return {acClaim.Actor, acClaim.Epoch, aSequence, aAction, acClaim.Where, acClaim.Lease};
}
} // namespace

TEST_CASE("Player furniture grants one player per marker and preserves their current seat if another is occupied", "[player_furniture]")
{
    Claims claims;
    const Authority a{7, 3, true};
    const Authority b{8, 4, true};

    const auto first = claims.Apply(Enter(a, 1, kChair), a, 100);
    REQUIRE(first.Decision == Verdict::Granted);
    REQUIRE(first.Current);
    CHECK(first.Current->State == Phase::Entering);
    CHECK(claims.Apply(Enter(b, 1, kChair), b, 101).Decision == Verdict::Occupied);

    const auto second = claims.Apply(Enter(b, 2, kOtherMarker), b, 102);
    REQUIRE(second.Decision == Verdict::Granted);
    REQUIRE(second.Current);
    const auto denied = claims.Apply(Enter(b, 3, kChair), b, 103);
    REQUIRE(denied.Current);
    CHECK(denied.Decision == Verdict::Occupied);
    CHECK(denied.Current->Where == kOtherMarker);
    CHECK_FALSE(denied.Released);

    const auto bed = claims.Apply(Enter(b, 4, kBed), b, 104);
    REQUIRE(bed.Current);
    REQUIRE(bed.Released);
    CHECK(bed.Released->Lease == second.Current->Lease);
    CHECK(bed.Current->Lease != bed.Released->Lease);
}

TEST_CASE("Player furniture refreshes settled claims and exposes them to later observers", "[player_furniture]")
{
    Claims claims;
    const Authority a{7, 3, true};
    const auto bed = claims.Apply(Enter(a, 1, kBed), a, 100);
    REQUIRE(bed.Current);

    const auto slept = claims.Apply(Change(*bed.Current, 2, Action::Sleep), a, 500);
    REQUIRE(slept.Decision == Verdict::Settled);
    REQUIRE(slept.Current);
    REQUIRE(claims.Find(a.Actor, 1000));
    CHECK(claims.Find(a.Actor, 1000)->State == Phase::Sleeping);

    CHECK(claims.Apply(Change(*slept.Current, 3, Action::Refresh), a, 5000).Decision == Verdict::Refreshed);
    CHECK(claims.Expire(6000).empty());
    const auto expired = claims.Expire(10001);
    REQUIRE(expired.size() == 1);
    CHECK(expired.front().Lease == bed.Current->Lease);
    CHECK_FALSE(claims.Find(a.Actor, 10001));
    CHECK(claims.Expire(10002).empty());
}

TEST_CASE("Player furniture rejects a former owner and a superseded lease", "[player_furniture]")
{
    Claims claims;
    const Authority a{7, 3, true};
    const Authority b{7, 4, true};
    const auto first = claims.Apply(Enter(a, 1, kChair), a, 100);
    REQUIRE(first.Current);

    CHECK(claims.Apply(Change(*first.Current, 2, Action::Refresh), b, 101).Decision == Verdict::Rejected);
    const auto second = claims.Apply(Enter(b, 1, kChair), b, 102);
    REQUIRE(second.Current);
    REQUIRE(second.Released);
    CHECK(second.Released->Epoch == a.Epoch);

    auto staleExit = Change(*second.Current, 2, Action::Exit);
    staleExit.Lease = first.Current->Lease;
    CHECK(claims.Apply(staleExit, b, 103).Decision == Verdict::Rejected);
    CHECK(claims.Find(b.Actor, 103));
    const auto stood = claims.Apply(Change(*second.Current, 3, Action::Exit), {b.Actor, b.Epoch, false}, 104);
    REQUIRE(stood.Decision == Verdict::Released);
    CHECK_FALSE(stood.Current);
    CHECK(claims.Apply(Enter(b, 1, kChair), b, 105).Decision == Verdict::Rejected);
}

TEST_CASE("Player furniture checks the server's authority and retires a claim on disconnect", "[player_furniture]")
{
    Claims claims;
    const Authority a{7, 3, true};
    const Authority b{8, 4, true};
    CHECK(claims.Apply(Enter(a, 1, kChair), b, 100).Decision == Verdict::Rejected);
    CHECK(claims.Apply(Enter(a, 1, kChair), {a.Actor, a.Epoch, false}, 100).Decision == Verdict::Rejected);
    CHECK(claims.Apply(Enter(a, 2, {{2, 0}, 0}), a, 101).Decision == Verdict::Rejected);
    CHECK(claims.Apply(Enter(a, 3, {kChair.Reference, NpcFurniture::kNoFurnitureMarker}), a, 102).Decision == Verdict::Rejected);

    const auto first = claims.Apply(Enter(a, 4, kChair), a, 103);
    REQUIRE(first.Current);
    const auto departed = claims.RemoveActor(a.Actor);
    REQUIRE(departed);
    CHECK(departed->Lease == first.Current->Lease);
    CHECK_FALSE(claims.Find(a.Actor, 104));
    CHECK(claims.Apply(Enter(b, 1, kChair), b, 105).Decision == Verdict::Granted);
}

TEST_CASE("A player furniture reservation stays bound to the checked cell", "[player_furniture]")
{
    Claims claims;
    const Authority cRoom{7, 3, true, {0, 32}};
    const Authority cHall{7, 3, true, {0, 33}};
    const auto cEntering = claims.Apply(Enter(cRoom, 1, kChair), cRoom, 100);
    REQUIRE(cEntering.Current);
    CHECK(claims.Apply(Change(*cEntering.Current, 2, Action::Refresh), cRoom, 101).Decision == Verdict::Rejected);
    CHECK(claims.Apply(Change(*cEntering.Current, 3, Action::Sit), cHall, 102).Decision == Verdict::Rejected);
    CHECK(claims.Apply(Change(*cEntering.Current, 4, Action::Sit), cRoom, 103).Decision == Verdict::Settled);
    REQUIRE(claims.ReleaseActor(cRoom.Actor));
    CHECK_FALSE(claims.Find(cRoom.Actor, 104));
    CHECK(claims.Apply(Enter(cHall, 4, kChair), cHall, 105).Decision == Verdict::Rejected);
    CHECK(claims.Apply(Enter(cHall, 5, kChair), cHall, 106).Decision == Verdict::Granted);
}

TEST_CASE("A cancelled in-flight grant releases only its own reservation", "[player_furniture]")
{
    Claims claims;
    const Authority cFirst{7, 3, true, {0, 32}};
    const Authority cSecond{8, 4, true, {0, 32}};
    REQUIRE(claims.Apply(Enter(cFirst, 1, kChair), cFirst, 100).Current);
    const auto cRetry = claims.Apply(Enter(cFirst, 2, kChair), cFirst, 150);
    REQUIRE(cRetry.Current);

    Request cancel{cFirst.Actor, cFirst.Epoch, 3, Action::Cancel, kChair, 0, 1};
    CHECK(claims.Apply(cancel, cFirst, 200).Decision == Verdict::Rejected);
    CHECK(claims.Apply(Enter(cSecond, 1, kChair), cSecond, 201).Decision == Verdict::Occupied);
    cancel.Sequence = 4;
    cancel.EnterSequence = 2;
    CHECK(claims.Apply(cancel, cFirst, 250).Decision == Verdict::Released);

    const auto cNext = claims.Apply(Enter(cSecond, 2, kChair), cSecond, 300);
    REQUIRE(cNext.Current);
    CHECK(claims.Apply(Change(*cNext.Current, 3, Action::Sit), cSecond, 400).Decision == Verdict::Settled);
    const Request cLateCancel{cSecond.Actor, cSecond.Epoch, 4, Action::Cancel, kChair, 0, 2};
    CHECK(claims.Apply(cLateCancel, cSecond, 500).Decision == Verdict::Rejected);
    CHECK(claims.Find(cSecond.Actor, 500));
}

TEST_CASE("An enter retry rejected by current authority frees its pending marker", "[player_furniture]")
{
    Claims claims;
    const Authority cFirst{7, 3, true, {0, 32}};
    const Authority cSecond{8, 4, true, {0, 32}};
    REQUIRE(claims.Apply(Enter(cFirst, 1, kChair), cFirst, 100).Current);
    const auto cRejected = claims.Apply(Enter(cFirst, 2, kChair), {cFirst.Actor, cFirst.Epoch, false, cFirst.Cell}, 150);
    CHECK(cRejected.Decision == Verdict::Rejected);
    CHECK(cRejected.Released);
    CHECK(claims.Apply(Enter(cSecond, 1, kChair), cSecond, 151).Decision == Verdict::Granted);
}

TEST_CASE("Player furniture messages preserve claims, replies and releases", "[player_furniture]")
{
    using namespace TiltedPhoques;
    RequestPlayerFurniture request;
    request.Request = {0x80000008, 3, 0x100000008, Action::Sleep, kBed, 0x100000009};
    request.Cell = {5, 0xB4D2};

    NotifyPlayerFurniture notify;
    notify.Actor = request.Request.Actor;
    notify.Epoch = request.Request.Epoch;
    notify.Where = request.Request.Where;
    notify.Cell = request.Cell;
    notify.State = SeatState::Sleeping;
    notify.Lease = request.Request.Lease;

    const auto cDecodeRequest = [](const RequestPlayerFurniture& acRequest)
    {
        Buffer buffer(256);
        Buffer::Writer writer(&buffer);
        acRequest.Serialize(writer);
        Buffer::Reader reader(&buffer);
        return ClientMessageFactory{}.Extract(reader);
    };
    auto pRequest = cDecodeRequest(request);
    REQUIRE(pRequest);
    REQUIRE(pRequest->GetOpcode() == RequestPlayerFurniture::Opcode);
    CHECK(*CastUnique<RequestPlayerFurniture>(std::move(pRequest)) == request);
    request.Request.Change = Action::Cancel;
    request.Request.Lease = 0;
    request.Request.EnterSequence = 0x100000007;
    pRequest = cDecodeRequest(request);
    REQUIRE(pRequest);
    CHECK(*CastUnique<RequestPlayerFurniture>(std::move(pRequest)) == request);
    request.Request.Where.Marker = kMarkerCount;
    CHECK_FALSE(cDecodeRequest(request));
    request.Request.Where = kBed;
    request.Request.Change = static_cast<Action>(255);
    CHECK_FALSE(cDecodeRequest(request));

    const auto cRoundTripNotice = [](const NotifyPlayerFurniture& acNotify)
    {
        Buffer buffer(256);
        Buffer::Writer writer(&buffer);
        acNotify.Serialize(writer);
        Buffer::Reader reader(&buffer);
        auto pNotify = ServerMessageFactory{}.Extract(reader);
        REQUIRE(pNotify);
        REQUIRE(pNotify->GetOpcode() == NotifyPlayerFurniture::Opcode);
        CHECK(*CastUnique<NotifyPlayerFurniture>(std::move(pNotify)) == acNotify);
    };
    cRoundTripNotice(notify);
    notify.State = SeatState::Standing;
    cRoundTripNotice(notify);
    notify.Sequence = request.Request.Sequence;
    notify.Decision = Verdict::Occupied;
    cRoundTripNotice(notify);
}

TEST_CASE("A marker index past the furniture's markers does not fit", "[player_furniture]")
{
    CHECK(MarkerFits(0));
    CHECK(MarkerFits(kMarkerCount - 1));
    CHECK_FALSE(MarkerFits(kMarkerCount));
    CHECK_FALSE(MarkerFits(0xFFFFFFFF));
}
