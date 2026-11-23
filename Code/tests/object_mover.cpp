#include <catch2/catch.hpp>

#include <ObjectMover.h>

using namespace std::chrono_literals;

TEST_CASE("only one player moves a loose object at a time", "[objects]")
{
    const ObjectMover::Clock::time_point start{};
    ObjectMover mover;

    REQUIRE(mover.Take(1, false, start));
    REQUIRE(mover.Take(1, false, start + 100ms));

    // The other player's push while the first still moves it is dropped.
    REQUIRE_FALSE(mover.Take(2, false, start + 500ms));
    REQUIRE(mover.GetPlayerId() == 1u);

    // Two seconds after the first player's last move, the other's game has let go of it too, and may take it.
    REQUIRE_FALSE(mover.Take(2, false, start + 100ms + ObjectMover::kHeldFor - 1ms));
    REQUIRE(mover.Take(2, false, start + 100ms + ObjectMover::kHeldFor));
    REQUIRE(mover.GetPlayerId() == 2u);
    REQUIRE_FALSE(mover.Take(1, false, start + 2500ms));
}

TEST_CASE("a player id of zero is a player like any other", "[objects]")
{
    const ObjectMover::Clock::time_point start{};
    ObjectMover mover;

    REQUIRE(mover.Take(0, false, start));
    REQUIRE_FALSE(mover.Take(1, false, start + 1s));
}

TEST_CASE("an object put down can be taken by another player after half a second", "[objects]")
{
    const ObjectMover::Clock::time_point start{};
    ObjectMover mover;

    REQUIRE(mover.Take(1, false, start));
    REQUIRE(mover.Take(1, true, start + 100ms));

    // The other game lets go of an object put down after half a second, not two.
    REQUIRE_FALSE(mover.Take(2, false, start + 100ms + ObjectMover::kHeldForAtRest - 1ms));
    REQUIRE(mover.Take(2, false, start + 100ms + ObjectMover::kHeldForAtRest));
    REQUIRE(mover.GetPlayerId() == 2u);
}

TEST_CASE("a move sent before the mover's last one is dropped, so it cannot undo a put-down", "[objects]")
{
    const ObjectMover::Clock::time_point start{};
    ObjectMover mover;

    REQUIRE(mover.Take(1, false, start, 1000));
    REQUIRE(mover.Take(1, true, start + 100ms, 1100));
    // A move that went unreliable before the put-down arrives after it.
    REQUIRE_FALSE(mover.Take(1, false, start + 120ms, 1066));
    // Still the put-down: another player may take it half a second after that, not two seconds after the late move.
    REQUIRE(mover.Take(2, false, start + 100ms + ObjectMover::kHeldForAtRest, 1700));
    // The first player's late moves count for nothing against the new mover's ticks, and after a lapse a new carry goes on.
    REQUIRE(mover.Take(1, false, start + 100ms + ObjectMover::kHeldForAtRest + ObjectMover::kHeldFor, 900));
    // Without ticks nothing is ordered.
    REQUIRE(mover.Take(1, false, start + 5s));
    REQUIRE(mover.Take(1, false, start + 5s + 33ms, 1));
}
