#include <catch2/catch.hpp>

#include <ArrowPickup.h>

#include <limits>
#include <string>

using ArrowPickup::Refusal;

TEST_CASE("The nearest copy of a taken arrow within reach of its place is removed", "[arrow_pickup]")
{
    using ArrowPickup::Candidate;
    const Candidate cCopies[] = {{25.f, true, true, false, true}, {4.f, true, true, false, true}, {12.f, true, true, false, true}};
    REQUIRE(ArrowPickup::PickNearest(cCopies, 3) == 1);

    // Exactly at the limit still counts; just past it does not.
    const Candidate cEdge[] = {{ArrowPickup::kMatchUnits, true, true, false, true}};
    REQUIRE(ArrowPickup::PickNearest(cEdge, 1) == 0);
    const Candidate cPast[] = {{ArrowPickup::kMatchUnits + 0.01f, true, true, false, true}, {400.f, true, true, false, true}};
    REQUIRE(ArrowPickup::PickNearest(cPast, 2) == 2);

    // No copy at all: nothing is removed.
    REQUIRE(ArrowPickup::PickNearest(nullptr, 0) == 0);

    // A distance that is not a number is never picked.
    const Candidate cNan[] = {{std::numeric_limits<float>::quiet_NaN(), true, true, false, true}, {20.f, true, true, false, true}};
    REQUIRE(ArrowPickup::PickNearest(cNan, 2) == 1);
}

TEST_CASE("Only a shot arrow of the same projectile, not with an actor, is removed", "[arrow_pickup]")
{
    using ArrowPickup::Candidate;
    // Another kind of arrow right there is left; the right kind further off is taken.
    const Candidate cOtherKind[] = {{1.f, false, true, false, true}, {20.f, true, true, false, true}};
    REQUIRE(ArrowPickup::PickNearest(cOtherKind, 2) == 1);

    // A placed reference of a plugin (not made at run time) is never taken.
    const Candidate cPlaced[] = {{1.f, true, false, false, true}};
    REQUIRE(ArrowPickup::PickNearest(cPlaced, 1) == 1);

    // An arrow in a body or with an actor is StuckArrows', never taken here, even when nearest.
    const Candidate cOnActor[] = {{2.f, true, true, true, true}, {28.f, true, true, false, true}};
    REQUIRE(ArrowPickup::PickNearest(cOnActor, 2) == 1);
    const Candidate cOnlyOnActor[] = {{2.f, true, true, true, true}};
    REQUIRE(ArrowPickup::PickNearest(cOnlyOnActor, 1) == 1);
}

TEST_CASE("An arrow within an actor's width and height counts as with that actor", "[arrow_pickup]")
{
    // In the chest of someone standing at the origin, and in a body lying there.
    REQUIRE(ArrowPickup::IsWithActor(10.f, -20.f, 120.f, 0.f, 0.f, 0.f));
    REQUIRE(ArrowPickup::IsWithActor(40.f, 30.f, 5.f, 0.f, 0.f, 0.f));
    // In the wall beside them, or high above them, it is not.
    REQUIRE_FALSE(ArrowPickup::IsWithActor(80.f, 0.f, 100.f, 0.f, 0.f, 0.f));
    REQUIRE_FALSE(ArrowPickup::IsWithActor(0.f, 0.f, 260.f, 0.f, 0.f, 0.f));
    REQUIRE_FALSE(ArrowPickup::IsWithActor(0.f, 0.f, -40.f, 0.f, 0.f, 0.f));
}

TEST_CASE("a player standing at an arrow never keeps it out of the pick-up", "[arrow_pickup]")
{
    // The one taking a wall arrow at chest height stands within reach of it, in its own game and as its copy in the others.
    REQUIRE_FALSE(ArrowPickup::KeptForActor(30.f, 0.f, 110.f, 0.f, 0.f, 0.f, true));
    // An NPC or a body at the same place still keeps it.
    REQUIRE(ArrowPickup::KeptForActor(30.f, 0.f, 110.f, 0.f, 0.f, 0.f, false));
    // Out of reach, nobody keeps it.
    REQUIRE_FALSE(ArrowPickup::KeptForActor(300.f, 0.f, 110.f, 0.f, 0.f, 0.f, false));
}

TEST_CASE("An exterior arrow's grid cell comes from its place", "[arrow_pickup]")
{
    REQUIRE(ArrowPickup::GridOf(0.f) == 0);
    REQUIRE(ArrowPickup::GridOf(4095.f) == 0);
    REQUIRE(ArrowPickup::GridOf(4096.f) == 1);
    REQUIRE(ArrowPickup::GridOf(-1.f) == -1);
    REQUIRE(ArrowPickup::GridOf(-4097.f) == -2);
}

TEST_CASE("The server takes a pick-up within an arm's reach of the player", "[arrow_pickup]")
{
    ArrowPickup::Check check{true, 100.f, 200.f, 50.f, true, 0.f, 0.f, 0.f};
    REQUIRE(ArrowPickup::Judge(check) == Refusal::kNone);

    check.X = ArrowPickup::kReachUnits + 1.f;
    check.Y = 0.f;
    check.Z = 0.f;
    REQUIRE(ArrowPickup::Judge(check) == Refusal::kTooFar);
}

TEST_CASE("The server refuses a pick-up with no arrow, no usable place or no known player", "[arrow_pickup]")
{
    ArrowPickup::Check check{false, 0.f, 0.f, 0.f, true, 0.f, 0.f, 0.f};
    REQUIRE(ArrowPickup::Judge(check) == Refusal::kNoArrow);

    check.HasArrow = true;
    check.X = std::numeric_limits<float>::infinity();
    REQUIRE(ArrowPickup::Judge(check) == Refusal::kNotAPlace);
    check.X = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(ArrowPickup::Judge(check) == Refusal::kNotAPlace);
    check.X = 2.0e7f;
    REQUIRE(ArrowPickup::Judge(check) == Refusal::kNotAPlace);

    check.X = 0.f;
    check.HasPicker = false;
    REQUIRE(ArrowPickup::Judge(check) == Refusal::kNoPicker);
    check.HasPicker = true;
    check.PickerY = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(ArrowPickup::Judge(check) == Refusal::kNoPicker);
}

TEST_CASE("A player sends at most eight pick-ups a second", "[arrow_pickup]")
{
    ArrowPickup::RateGate gate;
    for (uint32_t i = 0; i < ArrowPickup::kMaxPerSecond; ++i)
        REQUIRE(gate.Allow(5000 + i));
    REQUIRE_FALSE(gate.Allow(5500));
    // The next second starts afresh.
    REQUIRE(gate.Allow(6000));
    // A clock that stepped back starts afresh too.
    REQUIRE(gate.Allow(100));
}

TEST_CASE("Every refusal has words for the log", "[arrow_pickup]")
{
    for (Refusal refusal : {Refusal::kNone, Refusal::kNotAPlace, Refusal::kNoPicker, Refusal::kTooFar, Refusal::kTooOften, Refusal::kNoArrow, Refusal::kWrongCell})
        REQUIRE(std::string(ArrowPickup::Describe(refusal)) != "unknown");
}

TEST_CASE("An arrow still in flight is never taken for the one picked up", "[arrow_pickup]")
{
    using ArrowPickup::Candidate;
    // Nearest but moving: the landed one further off is taken.
    const Candidate cFlying[] = {{3.f, true, true, false, false}, {25.f, true, true, false, true}};
    REQUIRE(ArrowPickup::PickNearest(cFlying, 2) == 1);
    const Candidate cOnlyFlying[] = {{3.f, true, true, false, false}};
    REQUIRE(ArrowPickup::PickNearest(cOnlyFlying, 1) == 1);
}

TEST_CASE("An arrow is at rest when it did not move between two reads a moment apart", "[arrow_pickup]")
{
    REQUIRE(ArrowPickup::IsAtRest(10.f, 20.f, 30.f, 10.f, 20.f, 30.f));
    REQUIRE(ArrowPickup::IsAtRest(10.f, 20.f, 30.f, 10.5f, 20.f, 30.5f));
    // An arrow in flight covers far more than a unit in a tenth of a second.
    REQUIRE_FALSE(ArrowPickup::IsAtRest(10.f, 20.f, 30.f, 150.f, 20.f, 25.f));
    REQUIRE_FALSE(ArrowPickup::IsAtRest(0.f, 0.f, 0.f, std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f));

    REQUIRE_FALSE(ArrowPickup::RestReadDue(1000, 1000 + ArrowPickup::kRestWaitMs - 1));
    REQUIRE(ArrowPickup::RestReadDue(1000, 1000 + ArrowPickup::kRestWaitMs));
    // A clock that stepped back reads again at once.
    REQUIRE(ArrowPickup::RestReadDue(1000, 500));
}

TEST_CASE("Indoors the arrow must be in the player's own cell", "[arrow_pickup]")
{
    ArrowPickup::Check check{true, 100.f, 50.f, 0.f, true, 0.f, 0.f, 0.f};
    check.PickerInterior = true;
    check.SameCell = true;
    REQUIRE(ArrowPickup::Judge(check) == Refusal::kNone);
    check.SameCell = false;
    REQUIRE(ArrowPickup::Judge(check) == Refusal::kWrongCell);
}

TEST_CASE("Outdoors the arrow must be in the player's grid cell or one around it", "[arrow_pickup]")
{
    // The player near the edge of grid cell (0, 0); the arrow just over it, in (1, 0): taken, whatever cell id the message names.
    ArrowPickup::Check check{true, 4100.f, 100.f, 0.f, true, 4000.f, 100.f, 0.f};
    check.PickerInterior = false;
    check.SameCell = false;
    check.PickerGridX = 0;
    check.PickerGridY = 0;
    REQUIRE(ArrowPickup::Judge(check) == Refusal::kNone);
    // The server's grid cell for the player disagrees by two: refused.
    check.PickerGridX = -1;
    REQUIRE(ArrowPickup::Judge(check) == Refusal::kWrongCell);
}
