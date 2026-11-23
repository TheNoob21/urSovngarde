#include <catch2/catch.hpp>

#include <Systems/SeatPin.h>
#include <glm/gtc/constants.hpp>
#include <MountSync.h>

#include <cmath>
#include <limits>

TEST_CASE("A rider held on its horse keeps its seat however unevenly either's updates come", "[seat_pin]")
{
    // The seat its owner had it in, as seen from the horse.
    const glm::vec3 cSeat = SeatPin::Capture({105.f, 210.f, 40.f}, {100.f, 200.f, 0.f}, 0.f);
    REQUIRE(cSeat.x == Approx(5.f));
    REQUIRE(cSeat.y == Approx(10.f));
    REQUIRE(cSeat.z == Approx(40.f));

    // A galloping, turning horse drawn at uneven frames (8 to 70 ms apart), as packets come late or in bursts.
    const double cFrames[] = {8.0, 33.0, 70.0, 12.0, 45.0, 16.0, 61.0, 9.0, 33.0, 50.0, 22.0, 17.0};
    glm::vec3 horse{1000.f, -2000.f, 50.f};
    float heading = 0.3f;
    for (const double cFrame : cFrames)
    {
        const float cSeconds = static_cast<float>(cFrame / 1000.0);
        heading += 1.5f * cSeconds;
        // 800 units a second, the way the horse faces, over rough ground.
        horse += glm::vec3{std::sin(heading), std::cos(heading), 0.f} * (800.f * cSeconds);
        horse.z += 30.f * std::sin(static_cast<float>(cFrame));

        const glm::vec3 cRider = SeatPin::Place(horse, heading, cSeat);
        // No motion against the saddle: the same offset from the horse, and the same seat seen from it.
        REQUIRE(glm::distance(cRider, horse) == Approx(glm::length(cSeat)).margin(1e-3f));
        const glm::vec3 cSeen = SeatPin::ToHorse(cRider - horse, heading);
        REQUIRE(cSeen.x == Approx(cSeat.x).margin(1e-3f));
        REQUIRE(cSeen.y == Approx(cSeat.y).margin(1e-3f));
        REQUIRE(cSeen.z == Approx(cSeat.z).margin(1e-3f));
    }
}

TEST_CASE("The seat turns with the horse, as the game faces it", "[seat_pin]")
{
    // Heading 0 faces +y; a quarter turn faces +x. Ahead of the horse stays ahead of it.
    const glm::vec3 cAhead{0.f, 10.f, 0.f};
    const glm::vec3 cNorth = SeatPin::FromHorse(cAhead, 0.f);
    REQUIRE(cNorth.y == Approx(10.f));
    const glm::vec3 cEast = SeatPin::FromHorse(cAhead, glm::half_pi<float>());
    REQUIRE(cEast.x == Approx(10.f));
    REQUIRE(cEast.y == Approx(0.f).margin(1e-4f));
    // Right of the horse at a quarter turn is -y.
    const glm::vec3 cRight = SeatPin::FromHorse({10.f, 0.f, 0.f}, glm::half_pi<float>());
    REQUIRE(cRight.y == Approx(-10.f));
    REQUIRE(cRight.x == Approx(0.f).margin(1e-4f));
}

TEST_CASE("A seat too far from the horse, or not a number, holds the rider on the horse's place", "[seat_pin]")
{
    REQUIRE(SeatPin::Capture({0.f, SeatPin::kMaxSeatOffset + 1.f, 0.f}, {}, 0.f) == glm::vec3{});
    REQUIRE(SeatPin::Capture({std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f}, {}, 0.f) == glm::vec3{});
    REQUIRE(SeatPin::SeatWeight(0.0) == 0.f);
    REQUIRE(SeatPin::SeatWeight(SeatPin::kSeatEaseMs / 2.0) == Approx(0.5f));
    REQUIRE(SeatPin::SeatWeight(SeatPin::kSeatEaseMs) == 1.f);
}

TEST_CASE("The seat is taken once its owner's rider holds still on the horse, or soon after the copy sits", "[seat_pin]")
{
    REQUIRE_FALSE(SeatPin::SeatSettled(0.0, 0.0));
    REQUIRE_FALSE(SeatPin::SeatSettled(SeatPin::kSeatSettledMs - 1.0, 600.0));
    REQUIRE(SeatPin::SeatSettled(SeatPin::kSeatSettledMs, 300.0));
    REQUIRE(SeatPin::SeatSettled(10.0, SeatPin::kSeatLatestMs));
    REQUIRE(SeatPin::Holds({1.f, 2.f, 3.f}, 0.5f));
    REQUIRE_FALSE(SeatPin::Holds({std::numeric_limits<float>::infinity(), 2.f, 3.f}, 0.5f));
    REQUIRE_FALSE(SeatPin::Holds({1.f, 2.f, 3.f}, std::numeric_limits<float>::quiet_NaN()));
}

TEST_CASE("A copy let off its horse never glides below its owner's place", "[seat_pin]")
{
    REQUIRE(SeatPin::AboveGround({3.f, 4.f, -20.f}) == glm::vec3{3.f, 4.f, 0.f});
    REQUIRE(SeatPin::AboveGround({3.f, 4.f, 80.f}) == glm::vec3{3.f, 4.f, 80.f});
}

TEST_CASE("Mounted actions share the horse's playback tick until the pair separates", "[seat_pin]")
{
    CHECK(SeatPin::PlaybackTick(true, 740.5, 810.0) == 740.5);
    CHECK(SeatPin::PlaybackTick(false, 740.5, 810.0) == 810.0);
    CHECK(SeatPin::PlaybackTick(true, 0.0, 810.0) == 810.0);
    CHECK(SeatPin::PlaybackTick(true, 970.5, 810.0) == 970.5);
}

TEST_CASE("A seated rider uses its sampled heading on either side of a turn", "[seat_pin]")
{
    const float cTurning = glm::two_pi<float>() - 0.3f;
    CHECK(SeatPin::RiderHeading(cTurning, 0.2f, true, true) == 0.2f);
    CHECK(SeatPin::RiderHeading(0.2f, cTurning, true, true) == cTurning);
    CHECK(SeatPin::RiderHeading(cTurning, 0.2f, false, true) == cTurning);
    CHECK(SeatPin::RiderHeading(cTurning, 0.2f, true, false) == cTurning);
    CHECK(SeatPin::RiderHeading(cTurning, std::numeric_limits<float>::quiet_NaN(), true, true) == cTurning);
}

TEST_CASE("A rider getting on is said as getting on for its climb, and copies climb on with it", "[mount]")
{
    MountSync::Watch watch{};
    watch.StartedAt = 10.0;
    REQUIRE(MountSync::IsBoarding(watch, false, 10.5));
    // Taken over while already on: a refresh from the first word.
    REQUIRE_FALSE(MountSync::IsBoarding(watch, true, 10.5));
    watch.Sent = true;
    watch.FirstSentAt = 10.5;
    watch.SentAt = 10.5;
    REQUIRE(MountSync::IsBoarding(watch, false, 10.5 + MountSync::kClimbWindow - 0.01));
    REQUIRE_FALSE(MountSync::IsBoarding(watch, false, 10.5 + MountSync::kClimbWindow));

    // Said again while it climbs, then only at the refresh.
    REQUIRE(MountSync::NextWatchStep(watch, true, true, 10.5 + MountSync::kBoardingResendInterval) == MountSync::WatchStep::SendMount);
    REQUIRE(MountSync::NextWatchStep(watch, true, true, 10.5 + MountSync::kBoardingResendInterval / 2) == MountSync::WatchStep::Wait);
    watch.SentAt = 12.4;
    REQUIRE(MountSync::NextWatchStep(watch, true, true, 12.6) == MountSync::WatchStep::Wait);

    // The copy climbs on for a boarding word, or after replaying its owner's own climb, once only.
    REQUIRE(MountSync::ShowsClimb(true, -1.0, false));
    REQUIRE(MountSync::ShowsClimb(false, 0.4, false));
    REQUIRE_FALSE(MountSync::ShowsClimb(false, MountSync::kClimbTimeout, false));
    REQUIRE_FALSE(MountSync::ShowsClimb(false, -1.0, false));
    REQUIRE_FALSE(MountSync::ShowsClimb(true, -1.0, true));
}

TEST_CASE("Party members share horses; anybody else's stays theirs", "[mount]")
{
    MountSync::ClaimToRide claim{};
    claim.IsMount = true;
    claim.HasOwner = true;
    claim.OwnersInSameParty = true;
    claim.InRange = true;
    claim.EpochMatches = true;
    REQUIRE(MountSync::MayClaimToRide(claim));

    auto stranger = claim;
    stranger.OwnersInSameParty = false;
    REQUIRE_FALSE(MountSync::MayClaimToRide(stranger));
    auto ridden = claim;
    ridden.RiddenByOther = true;
    REQUIRE_FALSE(MountSync::MayClaimToRide(ridden));
    auto own = claim;
    own.OwnedBySender = true;
    REQUIRE_FALSE(MountSync::MayClaimToRide(own));
    auto old = claim;
    old.EpochMatches = false;
    REQUIRE_FALSE(MountSync::MayClaimToRide(old));
    auto distant = claim;
    distant.InRange = false;
    REQUIRE_FALSE(MountSync::MayClaimToRide(distant));
    auto dead = claim;
    dead.Dead = true;
    REQUIRE_FALSE(MountSync::MayClaimToRide(dead));
    auto person = claim;
    person.IsMount = false;
    REQUIRE_FALSE(MountSync::MayClaimToRide(person));

    REQUIRE(MountSync::SharesHorse(true, true, true));
    REQUIRE_FALSE(MountSync::SharesHorse(true, false, true));
    REQUIRE_FALSE(MountSync::SharesHorse(true, true, false));
}

TEST_CASE("With party horses off on the server, no claim to ride is taken whatever the games say", "[mount]")
{
    MountSync::ClaimToRide claim{};
    claim.IsMount = true;
    claim.HasOwner = true;
    claim.OwnersInSameParty = true;
    claim.InRange = true;
    claim.EpochMatches = true;
    // On unless the server says otherwise, as before the setting.
    REQUIRE(claim.SharingOn);
    REQUIRE(MountSync::MayClaimToRide(claim));

    // An enabled game asks; the server has sharing off: refused.
    auto off = claim;
    off.SharingOn = false;
    REQUIRE_FALSE(MountSync::MayClaimToRide(off));

    // Off refuses only: a claim that is refused anyway stays refused when it is turned back on.
    off.OwnersInSameParty = false;
    REQUIRE_FALSE(MountSync::MayClaimToRide(off));
    off.SharingOn = true;
    REQUIRE_FALSE(MountSync::MayClaimToRide(off));
}

TEST_CASE("With party horses off on the server, neither route gives a game a party member's horse, and a rider on its own horse is still told", "[mount]")
{
    // A party member's horse, as both server handlers see it: the claim to ride (OnOwnershipClaimRequest) and the mount request (OnMountRequest).
    MountSync::ClaimToRide claim{};
    claim.IsMount = claim.HasOwner = claim.OwnersInSameParty = claim.InRange = claim.EpochMatches = true;
    const MountSync::Identity cPair{12, 3, 34, 7};
    MountSync::RequestState foreign{};
    foreign.RiderOwnedBySender = foreign.OwnersInSameParty = foreign.IsMount = foreign.PairInRange = foreign.SenderInRange = true;
    REQUIRE(MountSync::MayClaimToRide(claim));
    REQUIRE(MountSync::MayRequest(cPair, 3, 7, foreign));

    // Sharing off: the claim to ride is refused, and the mount request that follows it (the client's mount watch) is refused too.
    claim.SharingOn = false;
    foreign.SharingOn = false;
    REQUIRE_FALSE(MountSync::MayClaimToRide(claim));
    REQUIRE_FALSE(MountSync::MayRequest(cPair, 3, 7, foreign));

    // The rider's own horse still goes through, so that the others seat its copy.
    MountSync::RequestState own = foreign;
    own.OwnersInSameParty = false;
    own.MountOwnedBySender = true;
    REQUIRE(MountSync::MayRequest(cPair, 3, 7, own));
    own.OwnersInSameParty = true;
    REQUIRE(MountSync::MayRequest(cPair, 3, 7, own));
}

TEST_CASE("A posed rider is held on its horse as the horse stands at the frame end", "[seat_pin]")
{
    const glm::vec3 cSeat{0.f, -10.f, 90.f};
    const glm::vec3 cStepPlace{100.f, 200.f, 390.f};
    // The horse moved on after the rider's step read it: the rider goes with the horse where it is now.
    const glm::vec3 cHorseNow{130.f, 240.f, 300.f};
    const glm::vec3 cPlaced = SeatPin::FrameEndPlace(true, cHorseNow, 0.f, cSeat, cStepPlace);
    CHECK(cPlaced.x == Approx(130.f));
    CHECK(cPlaced.y == Approx(230.f));
    CHECK(cPlaced.z == Approx(390.f));
    CHECK(glm::distance(cPlaced, SeatPin::Place(cHorseNow, 0.f, cSeat)) < 0.001f);
    // Turned a quarter, the seat turns with it.
    const glm::vec3 cTurned = SeatPin::FrameEndPlace(true, cHorseNow, 1.5707963f, cSeat, cStepPlace);
    CHECK(glm::distance(cTurned, SeatPin::Place(cHorseNow, 1.5707963f, cSeat)) < 0.001f);
    // The horse gone, or not a number: where the rider's step put it.
    CHECK(SeatPin::FrameEndPlace(false, cHorseNow, 0.f, cSeat, cStepPlace) == cStepPlace);
    CHECK(SeatPin::FrameEndPlace(true, glm::vec3{std::nanf(""), 0.f, 0.f}, 0.f, cSeat, cStepPlace) == cStepPlace);
}
