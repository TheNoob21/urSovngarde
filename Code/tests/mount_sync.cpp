#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <catch2/catch.hpp>
#include <MountSync.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/MountRequest.h>
#include <Messages/NotifyMount.h>
#include <Messages/RequestDismount.h>
#include <Messages/NotifyDismount.h>

TEST_CASE("mount packages require both current actor identities", "[mount]")
{
    const MountSync::Identity pair{12, 3, 34, 7};
    REQUIRE(MountSync::Current(pair, 3, 7));
    REQUIRE_FALSE(MountSync::Current(pair, 4, 7));
    REQUIRE_FALSE(MountSync::Current(pair, 3, 8));
    // Server id 0 is a real actor: the first character on a fresh server.
    REQUIRE(MountSync::Valid({0, 3, 34, 7}));
    REQUIRE(MountSync::Valid({12, 3, 0, 7}));
    REQUIRE_FALSE(MountSync::Valid({12, 0, 34, 7}));
    REQUIRE_FALSE(MountSync::Valid({12, 3, 34, 0}));
    REQUIRE_FALSE(MountSync::Valid({12, 3, 12, 7}));
}

TEST_CASE("a rider cannot take a dead or foreign party's horse through a package", "[mount]")
{
    const MountSync::Identity pair{12, 3, 34, 7};
    MountSync::RequestState valid{};
    valid.RiderOwnedBySender = valid.OwnersInSameParty = valid.IsMount = valid.PairInRange = valid.SenderInRange = true;
    REQUIRE(MountSync::MayRequest(pair, 3, 7, valid));
    for (auto field :
         {&MountSync::RequestState::RiderOwnedBySender, &MountSync::RequestState::OwnersInSameParty, &MountSync::RequestState::IsMount, &MountSync::RequestState::PairInRange,
          &MountSync::RequestState::SenderInRange})
    {
        auto refused = valid;
        refused.*field = false;
        REQUIRE_FALSE(MountSync::MayRequest(pair, 3, 7, refused));
    }
    for (auto field : {&MountSync::RequestState::RiderDead, &MountSync::RequestState::MountDead, &MountSync::RequestState::MountIsPlayer})
    {
        auto refused = valid;
        refused.*field = true;
        REQUIRE_FALSE(MountSync::MayRequest(pair, 3, 7, refused));
    }
    REQUIRE_FALSE(MountSync::MayRequest(pair, 4, 7, valid));
    REQUIRE_FALSE(MountSync::MayRequest(pair, 3, 8, valid));
    valid.OwnersInSameParty = false;
    valid.MountOwnedBySender = true;
    REQUIRE(MountSync::MayRequest(pair, 3, 7, valid));
    valid.RiderOwnedBySender = false;
    REQUIRE_FALSE(MountSync::MayRequest(pair, 3, 7, valid));
}

TEST_CASE("mount request and notification preserve both ownership epochs", "[mount][encoding.packets]")
{
    TiltedPhoques::Buffer requestBuffer(1000);
    MountRequest request{};
    request.RiderId = 12;
    request.MountId = 34;
    request.RiderOwnershipEpoch = 0xFFFFFFFF;
    request.MountOwnershipEpoch = 7;
    request.IsRefresh = true;
    TiltedPhoques::Buffer::Writer requestWriter(&requestBuffer);
    request.Serialize(requestWriter);
    TiltedPhoques::Buffer::Reader requestReader(&requestBuffer);
    auto requestMessage = ClientMessageFactory{}.Extract(requestReader);
    REQUIRE(requestMessage);
    REQUIRE(requestMessage->GetOpcode() == kMountRequest);
    auto decodedRequest = TiltedPhoques::CastUnique<MountRequest>(std::move(requestMessage));
    REQUIRE(*decodedRequest == request);

    TiltedPhoques::Buffer notifyBuffer(1000);
    NotifyMount notify{};
    notify.RiderId = request.RiderId;
    notify.MountId = request.MountId;
    notify.RiderOwnershipEpoch = request.RiderOwnershipEpoch;
    // The server transfers the horse before notification; it carries the new epoch.
    notify.MountOwnershipEpoch = request.MountOwnershipEpoch + 1;
    notify.IsRefresh = true;
    TiltedPhoques::Buffer::Writer notifyWriter(&notifyBuffer);
    notify.Serialize(notifyWriter);
    TiltedPhoques::Buffer::Reader notifyReader(&notifyBuffer);
    auto notifyMessage = ServerMessageFactory{}.Extract(notifyReader);
    REQUIRE(notifyMessage);
    REQUIRE(notifyMessage->GetOpcode() == kNotifyMount);
    auto decodedNotify = TiltedPhoques::CastUnique<NotifyMount>(std::move(notifyMessage));
    REQUIRE(*decodedNotify == notify);
    const MountSync::Identity pair{notify.RiderId, notify.RiderOwnershipEpoch, notify.MountId, notify.MountOwnershipEpoch};
    REQUIRE_FALSE(MountSync::Current(pair, request.RiderOwnershipEpoch, request.MountOwnershipEpoch));
    REQUIRE(MountSync::Current(pair, notify.RiderOwnershipEpoch, notify.MountOwnershipEpoch));
    auto changed = notify;
    ++changed.MountOwnershipEpoch;
    REQUIRE_FALSE(changed == notify);
    changed = notify;
    --changed.RiderOwnershipEpoch;
    REQUIRE_FALSE(changed == notify);
    changed = notify;
    changed.IsRefresh = false;
    REQUIRE_FALSE(changed == notify);
}

TEST_CASE("the rider's game says what it reads back, not what the package meant", "[mount]")
{
    using MountSync::WatchStep;
    MountSync::Watch watch{};
    watch.StartedAt = 100.0;

    // Boarding: nothing goes out until the game says the rider is on, and only once both ids exist.
    REQUIRE(MountSync::NextWatchStep(watch, false, true, 101.0) == WatchStep::Wait);
    REQUIRE(MountSync::NextWatchStep(watch, true, false, 101.0) == WatchStep::Wait);
    // A rider really on the horse waits for its ids however long it takes.
    REQUIRE(MountSync::NextWatchStep(watch, true, false, 100.0 + MountSync::kBoardingTimeout * 4) == WatchStep::Wait);
    REQUIRE(MountSync::NextWatchStep(watch, true, true, 101.0) == WatchStep::SendMount);

    // A package that never put the rider on is dropped without a word.
    REQUIRE(MountSync::NextWatchStep(watch, false, true, 100.0 + MountSync::kBoardingTimeout) == WatchStep::Drop);
    REQUIRE(MountSync::NextWatchStep(watch, false, false, 100.0 + MountSync::kBoardingTimeout) == WatchStep::Drop);

    // On: said again every refresh interval, not every look.
    watch.Sent = true;
    watch.SentAt = 101.0;
    REQUIRE(MountSync::NextWatchStep(watch, true, true, 101.0 + MountSync::kWatchInterval) == WatchStep::Wait);
    REQUIRE(MountSync::NextWatchStep(watch, true, true, 101.0 + MountSync::kRefreshInterval) == WatchStep::SendMount);
    // Ids lost for a moment (a hand-over in flight): no refresh, no dismount.
    REQUIRE(MountSync::NextWatchStep(watch, true, false, 101.0 + MountSync::kRefreshInterval) == WatchStep::Wait);

    // Off after saying on: one look is not enough (a cell change can read off for a moment)...
    watch.OffReads = 1;
    REQUIRE(MountSync::NextWatchStep(watch, false, true, 101.0 + MountSync::kWatchInterval) == WatchStep::Wait);
    watch.OffReads = MountSync::kOffReadsToDismount - 1;
    REQUIRE(MountSync::NextWatchStep(watch, false, true, 101.0 + MountSync::kWatchInterval) == WatchStep::Wait);
    // ...but a few in a row tell the others, whatever took the rider off, ids known or not.
    watch.OffReads = MountSync::kOffReadsToDismount;
    REQUIRE(MountSync::NextWatchStep(watch, false, true, 101.0 + MountSync::kWatchInterval) == WatchStep::SendDismount);
    REQUIRE(MountSync::NextWatchStep(watch, false, false, 101.0 + MountSync::kWatchInterval) == WatchStep::SendDismount);
    // The dismount goes out in well under a second.
    STATIC_REQUIRE(MountSync::kOffReadsToDismount * MountSync::kWatchInterval < 1.0);
}

TEST_CASE("a copy is seated by read-back, a few times at most, and gets off when its rider goes quiet", "[mount]")
{
    using MountSync::SeatStep;
    MountSync::Seat seat{};
    seat.ToldAt = 200.0;

    // Nothing is tried on a pair that is not ready (not loaded, dead, other cell).
    REQUIRE(MountSync::NextSeatStep(seat, false, false, 200.0) == SeatStep::Wait);
    REQUIRE(MountSync::NextSeatStep(seat, true, false, 200.0) == SeatStep::Try);

    seat.Tries = 1;
    seat.TriedAt = 200.0;
    REQUIRE(MountSync::NextSeatStep(seat, true, false, 200.0 + MountSync::kSeatRetryInterval / 2) == SeatStep::Wait);
    REQUIRE(MountSync::NextSeatStep(seat, true, false, 200.0 + MountSync::kSeatRetryInterval) == SeatStep::Try);

    // The game says it is on: seated, and then left alone.
    REQUIRE(MountSync::NextSeatStep(seat, true, true, 201.0) == SeatStep::Seated);
    seat.Seated = true;
    REQUIRE(MountSync::NextSeatStep(seat, true, true, 202.0) == SeatStep::Wait);
    // Taken off here without word from the owner: put back with the tries left.
    REQUIRE(MountSync::NextSeatStep(seat, true, false, 202.0) == SeatStep::Unseated);
    seat.Seated = false;

    // Out of tries: left on foot, and not tried again for this pair.
    seat.Tries = MountSync::kSeatTries;
    seat.TriedAt = 203.0;
    // The last try gets its full interval too.
    REQUIRE(MountSync::NextSeatStep(seat, true, false, 203.0 + MountSync::kSeatRetryInterval / 2) == SeatStep::Wait);
    REQUIRE(MountSync::NextSeatStep(seat, true, false, 203.0 + MountSync::kSeatRetryInterval) == SeatStep::GiveUp);
    seat.GaveUp = true;
    REQUIRE(MountSync::NextSeatStep(seat, true, false, 211.0) == SeatStep::Wait);

    // No word from the rider's game for too long: off, seated or not.
    REQUIRE(MountSync::NextSeatStep(seat, true, false, 200.0 + MountSync::kSeatStaleAfter) == SeatStep::Release);
    seat.Seated = true;
    REQUIRE(MountSync::NextSeatStep(seat, true, true, 200.0 + MountSync::kSeatStaleAfter) == SeatStep::Release);
    // A refresh keeps it on.
    seat.ToldAt = 200.0 + MountSync::kSeatStaleAfter;
    REQUIRE(MountSync::NextSeatStep(seat, true, true, 200.0 + MountSync::kSeatStaleAfter + 1.0) == SeatStep::Wait);

    // The owner refreshes faster than a copy goes stale, and a seat gives up before its first refresh is due.
    STATIC_REQUIRE(MountSync::kRefreshInterval * 2 < MountSync::kSeatStaleAfter);
    STATIC_REQUIRE(MountSync::kSeatTries * MountSync::kSeatRetryInterval <= MountSync::kRefreshInterval);
}

TEST_CASE("only the rider's current owner can take it off its horse", "[mount]")
{
    REQUIRE(MountSync::MayDismount(3, 3, true));
    REQUIRE_FALSE(MountSync::MayDismount(3, 3, false));
    REQUIRE_FALSE(MountSync::MayDismount(3, 4, true));
    REQUIRE_FALSE(MountSync::MayDismount(0, 0, true));
    REQUIRE(MountSync::MayDismount(3, 3, true));
}

TEST_CASE("dismount request and notification round trip", "[mount][encoding.packets]")
{
    {
        TiltedPhoques::Buffer buffer(1000);
        RequestDismount request{};
        request.RiderId = 12;
        request.RiderOwnershipEpoch = 0xFFFFFFFF;
        request.MountId = 34;
        TiltedPhoques::Buffer::Writer writer(&buffer);
        request.Serialize(writer);
        TiltedPhoques::Buffer::Reader reader(&buffer);
        auto message = ClientMessageFactory{}.Extract(reader);
        REQUIRE(message);
        REQUIRE(message->GetOpcode() == kRequestDismount);
        REQUIRE(*TiltedPhoques::CastUnique<RequestDismount>(std::move(message)) == request);
    }
    {
        TiltedPhoques::Buffer buffer(1000);
        NotifyDismount notify{};
        notify.RiderId = 12;
        notify.RiderOwnershipEpoch = 3;
        notify.MountId = 0xFFFFFFFF;
        TiltedPhoques::Buffer::Writer writer(&buffer);
        notify.Serialize(writer);
        TiltedPhoques::Buffer::Reader reader(&buffer);
        auto message = ServerMessageFactory{}.Extract(reader);
        REQUIRE(message);
        REQUIRE(message->GetOpcode() == kNotifyDismount);
        auto decoded = TiltedPhoques::CastUnique<NotifyDismount>(std::move(message));
        REQUIRE(*decoded == notify);
        auto changed = notify;
        ++changed.RiderOwnershipEpoch;
        REQUIRE_FALSE(changed == notify);
    }
}

TEST_CASE("a copy is seated only in the game's own mount range", "[mount]")
{
    using namespace MountSync;
    // On its owner's saddle, or beside the horse.
    REQUIRE(InSeatRange(0.f));
    REQUIRE(InSeatRange(150.f * 150.f));
    // The game's own limit is in range; past it the game refuses, so no try is spent.
    REQUIRE(InSeatRange(kSeatRange * kSeatRange));
    REQUIRE_FALSE(InSeatRange(3001.f * 3001.f));
    // Not a distance (an unread position).
    REQUIRE_FALSE(InSeatRange(-1.f));

    // A copy out of range waits, with its tries kept.
    Seat seat{};
    seat.ToldAt = 10.0;
    REQUIRE(NextSeatStep(seat, InSeatRange(5000.f * 5000.f), false, 11.0) == SeatStep::Wait);
    REQUIRE(NextSeatStep(seat, InSeatRange(100.f * 100.f), false, 11.0) == SeatStep::Try);
}

TEST_CASE("a rider read off is told off within a few hundred ms, but not on one read", "[mount]")
{
    using MountSync::WatchStep;
    MountSync::Watch watch{};
    watch.StartedAt = 100.0;
    // Boarding and just read off are looked at every frame.
    REQUIRE(MountSync::ReadEveryFrame(watch));
    watch.Sent = true;
    watch.SentAt = 101.0;
    REQUIRE_FALSE(MountSync::ReadEveryFrame(watch));
    watch.OffReads = 1;
    watch.OffSince = 110.0;
    REQUIRE(MountSync::ReadEveryFrame(watch));

    // Three frame reads in 33 ms are not enough: one hitch is not a dismount.
    watch.OffReads = MountSync::kOffReadsToDismount;
    REQUIRE(MountSync::NextWatchStep(watch, false, true, 110.033) == WatchStep::Wait);
    REQUIRE(MountSync::NextWatchStep(watch, false, true, 110.0 + MountSync::kOffTimeToDismount) == WatchStep::SendDismount);
    // Both count: a long span of one read waits for the others.
    watch.OffReads = 1;
    REQUIRE(MountSync::NextWatchStep(watch, false, true, 112.0) == WatchStep::Wait);
    STATIC_REQUIRE(MountSync::kOffTimeToDismount < MountSync::kOffReadsToDismount * MountSync::kWatchInterval);
}

TEST_CASE("a copy shown climbing on is in the saddle by the climb, or put there", "[mount]")
{
    using MountSync::ClimbStep;
    REQUIRE(MountSync::NextClimbStep(MountSync::kSitStateSeated, 0.5) == ClimbStep::Done);
    REQUIRE(MountSync::NextClimbStep(2, 0.5) == ClimbStep::Wait);
    REQUIRE(MountSync::NextClimbStep(0, MountSync::kClimbTimeout - 0.01) == ClimbStep::Wait);
    REQUIRE(MountSync::NextClimbStep(2, MountSync::kClimbTimeout) == ClimbStep::Instant);
    REQUIRE(MountSync::NextClimbStep(MountSync::kSitStateSeated, MountSync::kClimbTimeout) == ClimbStep::Done);

    REQUIRE(MountSync::ExitDone(MountSync::kSitStateNone, 0.1));
    REQUIRE_FALSE(MountSync::ExitDone(4, 1.0));
    REQUIRE(MountSync::ExitDone(4, MountSync::kExitTimeout));
}

TEST_CASE("a copy climbs on at any try while its rider's getting on is fresh, not only the first", "[mount]")
{
    MountSync::Seat seat{};
    seat.ShowClimb = true;
    seat.ToldAt = 10.0;
    seat.BoardingAt = 10.0;

    // The first try climbs on, however long the pair took to be ready here.
    seat.Tries = 1;
    seat.TriedAt = 10.0;
    REQUIRE(MountSync::ClimbsOnTry(seat, 10.0));
    REQUIRE(MountSync::ClimbsOnTry(seat, 10.0 + 2 * MountSync::kClimbWindow));

    // The game refused it: the next one, a second later, still climbs on.
    REQUIRE(MountSync::NextSeatStep(seat, true, false, 10.0 + MountSync::kSeatRetryInterval) == MountSync::SeatStep::Try);
    seat.Tries = 2;
    REQUIRE(MountSync::ClimbsOnTry(seat, 10.0 + MountSync::kSeatRetryInterval));

    // A later try too late to be its rider's climb, or the word was a refresh: put in the saddle.
    REQUIRE_FALSE(MountSync::ClimbsOnTry(seat, 10.0 + MountSync::kClimbWindow));
    MountSync::Seat refresh = seat;
    refresh.ShowClimb = false;
    REQUIRE_FALSE(MountSync::ClimbsOnTry(refresh, 10.5));

    // Seated once: thrown off and put back, it does not climb on again.
    MountSync::Seat seated = seat;
    seated.Seated = true;
    REQUIRE_FALSE(MountSync::ClimbsOnTry(seated, 10.5));

    // A ride that starts over (its horse made again) shows no climb.
    MountSync::Restart(seat);
    REQUIRE_FALSE(MountSync::ClimbsOnTry(seat, 10.5));
}

TEST_CASE("a copy that gave up is tried once more on a refresh", "[mount]")
{
    using MountSync::SeatStep;
    MountSync::Seat seat{};
    seat.ToldAt = 300.0;
    seat.Tries = MountSync::kSeatTries;
    seat.TriedAt = 302.0;
    seat.GaveUp = true;
    REQUIRE(MountSync::NextSeatStep(seat, true, false, 305.0) == SeatStep::Wait);

    seat.ToldAt = 305.0;
    MountSync::RetryAfterRefresh(seat);
    REQUIRE_FALSE(seat.GaveUp);
    REQUIRE(MountSync::NextSeatStep(seat, true, false, 305.0) == SeatStep::Try);
    ++seat.Tries;
    seat.TriedAt = 305.0;
    REQUIRE(MountSync::NextSeatStep(seat, true, false, 305.0 + MountSync::kSeatRetryInterval) == SeatStep::GiveUp);

    // A seat that did not give up keeps its tries.
    MountSync::Seat trying{};
    trying.Tries = 1;
    MountSync::RetryAfterRefresh(trying);
    REQUIRE(trying.Tries == 1);
}

TEST_CASE("the server tells a late loader of a ride said lately under the current epochs", "[mount]")
{
    MountSync::Ride ride{};
    ride.Mount = 9;
    ride.RiderEpoch = 3;
    ride.MountEpoch = 5;
    ride.ToldAt = 100.0;
    REQUIRE(MountSync::TellLateLoader(ride, 3, 5, 104.0));
    REQUIRE_FALSE(MountSync::TellLateLoader(ride, 4, 5, 104.0));
    REQUIRE_FALSE(MountSync::TellLateLoader(ride, 3, 6, 104.0));
    REQUIRE_FALSE(MountSync::TellLateLoader(ride, 3, 5, 100.0 + MountSync::kSeatStaleAfter));
    REQUIRE_FALSE(MountSync::TellLateLoader(ride, 3, 5, 99.0));
    REQUIRE_FALSE(MountSync::TellLateLoader(MountSync::Ride{}, 0, 0, 100.0));
}

TEST_CASE("a copy's ride is held while its rider or horse changes owner, and carried on at the new epochs", "[mount]")
{
    using MountSync::BindingStep;
    MountSync::Binding bound{3, 5, 0x100, 0x200};
    REQUIRE(MountSync::CheckBinding(bound, 3, 5, 0x100, 0x200) == BindingStep::Keep);

    // The viewer stays a viewer: the rider's owner hands it to a third game. Nothing is done until the new owner says it rides on.
    REQUIRE(MountSync::CheckBinding(bound, 4, 5, 0x100, 0x200) == BindingStep::Hold);
    REQUIRE(MountSync::CheckBinding(bound, 3, 6, 0x100, 0x200) == BindingStep::Hold);
    REQUIRE(MountSync::Rebind(bound, 4, 5));
    REQUIRE(MountSync::CheckBinding(bound, 4, 5, 0x100, 0x200) == BindingStep::Keep);
    // The same epochs again: nothing withdrawn.
    REQUIRE_FALSE(MountSync::Rebind(bound, 4, 5));

    // Not tried on yet, or not loaded here now: kept.
    REQUIRE(MountSync::CheckBinding(MountSync::Binding{4, 5, 0, 0}, 4, 5, 0x100, 0x200) == BindingStep::Keep);
    REQUIRE(MountSync::CheckBinding(bound, 4, 5, 0, 0x200) == BindingStep::Keep);
}

TEST_CASE("a copy's ride starts over when its horse is replaced before a refresh", "[mount]")
{
    using MountSync::BindingStep;
    const MountSync::Binding cBound{3, 5, 0x100, 0x200};
    REQUIRE(MountSync::CheckBinding(cBound, 3, 5, 0x100, 0x300) == BindingStep::Restart);
    REQUIRE(MountSync::CheckBinding(cBound, 3, 5, 0x101, 0x200) == BindingStep::Restart);

    // The tries, the read-back and the climb go; the last word's time stays, so a silent owner still lets the copy go.
    MountSync::Seat seat{};
    seat.ToldAt = 40.0;
    seat.Tries = MountSync::kSeatTries;
    seat.GaveUp = true;
    seat.Seated = true;
    seat.ShowClimb = true;
    seat.Climbing = true;
    MountSync::Restart(seat);
    REQUIRE(seat.ToldAt == 40.0);
    REQUIRE(seat.Tries == 0);
    REQUIRE_FALSE(seat.GaveUp);
    REQUIRE_FALSE(seat.Seated);
    REQUIRE_FALSE(seat.ShowClimb);
    REQUIRE_FALSE(seat.Climbing);
    REQUIRE(MountSync::NextSeatStep(seat, true, false, 41.0) == MountSync::SeatStep::Try);
}

TEST_CASE("a rider found already on a horse is watched on the nearest ridden horse", "[mount_sync]")
{
    using MountSync::NearHorse;
    // Its own horse, and another rider's horse beside it.
    const NearHorse cTwo[] = {{0x200, 90.f, true, false}, {0x100, 4.f, true, false}};
    REQUIRE(MountSync::HorseRiddenHere(cTwo, 2) == 0x100);
    // A horse nobody rides, even the nearest, is not it.
    const NearHorse cIdle[] = {{0x300, 2.f, false, false}, {0x100, 6.f, true, false}};
    REQUIRE(MountSync::HorseRiddenHere(cIdle, 2) == 0x100);
    // Nor one another rider of this game is watched on.
    const NearHorse cTaken[] = {{0x100, 3.f, true, true}};
    REQUIRE(MountSync::HorseRiddenHere(cTaken, 1) == 0);
    // Nor one too far off to be under the rider.
    const NearHorse cFar[] = {{0x100, MountSync::kAlreadyOnUnits + 1.f, true, false}};
    REQUIRE(MountSync::HorseRiddenHere(cFar, 1) == 0);
    REQUIRE(MountSync::HorseRiddenHere(nullptr, 0) == 0);
}
