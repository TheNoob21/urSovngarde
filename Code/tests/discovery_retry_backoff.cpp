#include <catch2/catch.hpp>

#include <Services/DiscoveryRetryBackoff.h>

using Clock = DiscoveryRetryBackoff::Clock;
using namespace std::chrono_literals;

namespace
{
// An exterior place, and another one a grid cell away; and an interior.
const DiscoveryPlace cPlaceA{0x3C, 5, -11, 0};
const DiscoveryPlace cPlaceB{0x3C, 6, -11, 0};
const DiscoveryPlace cInterior{0, 0, 0, 0x16B8A};
} // namespace

TEST_CASE("A visit that never failed may always go", "[discovery]")
{
    DiscoveryRetryBackoff backoff;
    const auto t0 = Clock::now();
    for (int i = 0; i < 100; ++i)
        REQUIRE(backoff.MayVisit(t0 + i * 16ms, cPlaceA));
}

TEST_CASE("A failed visit waits before the next one in the same place instead of retrying on every frame", "[discovery]")
{
    DiscoveryRetryBackoff backoff;
    const auto t0 = Clock::now();
    backoff.NoteFailure(t0, cPlaceA);

    int visits = 0;
    for (int i = 1; i < 15; ++i) // the frames of the first 240 ms at 60 fps
        visits += backoff.MayVisit(t0 + i * 16ms, cPlaceA) ? 1 : 0;
    REQUIRE(visits == 0);

    REQUIRE_FALSE(backoff.MayVisit(t0 + DiscoveryRetryBackoff::cFirstInterval - 1ms, cPlaceA));
    REQUIRE(backoff.MayVisit(t0 + DiscoveryRetryBackoff::cFirstInterval, cPlaceA));
}

TEST_CASE("A new place is visited at once, however long the old one has been failing", "[discovery]")
{
    DiscoveryRetryBackoff backoff;
    auto now = Clock::now();
    for (int i = 0; i < 10; ++i) // the wait has climbed to its cap
    {
        backoff.NoteFailure(now, cPlaceA);
        now += DiscoveryRetryBackoff::cMaxInterval;
    }
    backoff.NoteFailure(now, cPlaceA);

    REQUIRE_FALSE(backoff.MayVisit(now + 1ms, cPlaceA));
    // The player walked on: another grid cell, another world space, or a door to an interior.
    REQUIRE(backoff.MayVisit(now + 1ms, cPlaceB));
    REQUIRE(backoff.MayVisit(now + 1ms, DiscoveryPlace{0x2A, 5, -11, 0}));
    REQUIRE(backoff.MayVisit(now + 1ms, cInterior));
}

TEST_CASE("A failure in a new place starts over from the short wait", "[discovery]")
{
    DiscoveryRetryBackoff backoff;
    auto now = Clock::now();
    for (int i = 0; i < 6; ++i)
        backoff.NoteFailure(now, cPlaceA);
    REQUIRE(backoff.Streak() == 6u);

    backoff.NoteFailure(now, cPlaceB);
    REQUIRE(backoff.Streak() == 1u);
    REQUIRE_FALSE(backoff.MayVisit(now + DiscoveryRetryBackoff::cFirstInterval - 1ms, cPlaceB));
    REQUIRE(backoff.MayVisit(now + DiscoveryRetryBackoff::cFirstInterval, cPlaceB));
}

TEST_CASE("Each failure in a row in the same place doubles the wait", "[discovery]")
{
    DiscoveryRetryBackoff backoff;
    auto now = Clock::now();
    auto expected = DiscoveryRetryBackoff::cFirstInterval;
    for (int i = 0; i < 4; ++i)
    {
        backoff.NoteFailure(now, cPlaceA);
        REQUIRE_FALSE(backoff.MayVisit(now + expected - 1ms, cPlaceA));
        REQUIRE(backoff.MayVisit(now + expected, cPlaceA));
        now += expected;
        expected *= 2;
    }
    REQUIRE(backoff.Streak() == 4u);
}

TEST_CASE("The wait never grows past its cap", "[discovery]")
{
    DiscoveryRetryBackoff backoff;
    auto now = Clock::now();
    for (int i = 0; i < 40; ++i)
    {
        backoff.NoteFailure(now, cPlaceA);
        now += DiscoveryRetryBackoff::cMaxInterval;
    }
    backoff.NoteFailure(now, cPlaceA);
    REQUIRE_FALSE(backoff.MayVisit(now + DiscoveryRetryBackoff::cMaxInterval - 1ms, cPlaceA));
    REQUIRE(backoff.MayVisit(now + DiscoveryRetryBackoff::cMaxInterval, cPlaceA));
}

TEST_CASE("A visit that worked clears the wait", "[discovery]")
{
    DiscoveryRetryBackoff backoff;
    const auto t0 = Clock::now();
    backoff.NoteFailure(t0, cPlaceA);
    backoff.NoteFailure(t0 + 250ms, cPlaceA);
    REQUIRE_FALSE(backoff.MayVisit(t0 + 300ms, cPlaceA));
    backoff.NoteSuccess();
    REQUIRE(backoff.MayVisit(t0 + 300ms, cPlaceA));
    REQUIRE(backoff.Streak() == 0u);

    // The next failure starts from the short wait again.
    backoff.NoteFailure(t0 + 400ms, cPlaceA);
    REQUIRE(backoff.MayVisit(t0 + 400ms + DiscoveryRetryBackoff::cFirstInterval, cPlaceA));
}

TEST_CASE("The failure total only grows, so a visit can tell whether it failed", "[discovery]")
{
    DiscoveryRetryBackoff backoff;
    const auto t0 = Clock::now();
    backoff.NoteFailure(t0, cPlaceA);
    const uint32_t cBefore = backoff.TotalFailures();
    backoff.NoteFailure(t0, cPlaceB); // the streak restarts for the new place, the total does not
    REQUIRE(backoff.Streak() == 1u);
    REQUIRE(backoff.TotalFailures() == cBefore + 1);
    backoff.NoteSuccess();
    REQUIRE(backoff.TotalFailures() == cBefore + 1);
}
