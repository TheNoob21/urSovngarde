#include <catch2/catch.hpp>

#include <Services/DiscoveryFailureLog.h>
#include <Services/DiscoveryRetryBackoff.h>

using Clock = DiscoveryRetryBackoff::Clock;
using namespace std::chrono_literals;

namespace
{
const DiscoveryPlace cPlaceA{0x3C, 5, -11, 0};
const DiscoveryPlace cPlaceB{0x3C, 6, -11, 0};
} // namespace

TEST_CASE("The first visit in a place logs each form that failed once as an error", "[discovery]")
{
    DiscoveryFailureLog log;
    log.BeginVisit(false);

    REQUIRE(log.ShouldLogError(0x9712));
    REQUIRE(log.ShouldLogError(0x9713));
    REQUIRE(log.ShouldLogError(0x9714));
}

TEST_CASE("A form met twice in one visit is an error once", "[discovery]")
{
    // The player's own cell is also one of the 25 cells of the grid.
    DiscoveryFailureLog log;
    log.BeginVisit(false);

    REQUIRE(log.ShouldLogError(0x9712));
    REQUIRE_FALSE(log.ShouldLogError(0x9712));
    REQUIRE(log.ShouldLogError(0x9713));
    REQUIRE_FALSE(log.ShouldLogError(0x9712));
}

TEST_CASE("The retry of a place that already failed logs no errors", "[discovery]")
{
    DiscoveryFailureLog log;
    log.BeginVisit(true);

    for (uint32_t id = 0x96F0; id < 0x96F0 + 26; ++id)
        REQUIRE_FALSE(log.ShouldLogError(id));
}

TEST_CASE("Every visit starts with no form seen", "[discovery]")
{
    DiscoveryFailureLog log;
    log.BeginVisit(false);
    REQUIRE(log.ShouldLogError(0x50008D3));
    REQUIRE_FALSE(log.ShouldLogError(0x50008D3));

    // The same form in another place, or after the failure healed, is reported again.
    log.BeginVisit(false);
    REQUIRE(log.ShouldLogError(0x50008D3));
}

TEST_CASE("The error lines of a failing place stop after its first visit", "[discovery]")
{
    // The visits of a player who stays where the server id of every cell is unknown (before the mod list arrives).
    DiscoveryRetryBackoff backoff;
    DiscoveryFailureLog log;
    auto now = Clock::now();

    int errors = 0;
    for (int visit = 0; visit < 20; ++visit)
    {
        REQUIRE(backoff.MayVisit(now, cPlaceA));
        log.BeginVisit(backoff.IsFailingIn(cPlaceA));
        for (uint32_t id = 0x96F0; id < 0x96F0 + 26; ++id)
            errors += log.ShouldLogError(id) ? 1 : 0;
        backoff.NoteFailure(now, cPlaceA);
        now += DiscoveryRetryBackoff::cMaxInterval;
    }

    REQUIRE(errors == 26);
}

TEST_CASE("A new place logs its failures again, and so does one after a visit that worked", "[discovery]")
{
    DiscoveryRetryBackoff backoff;
    DiscoveryFailureLog log;
    const auto t0 = Clock::now();

    REQUIRE_FALSE(backoff.IsFailingIn(cPlaceA));
    backoff.NoteFailure(t0, cPlaceA);
    REQUIRE(backoff.IsFailingIn(cPlaceA));
    REQUIRE_FALSE(backoff.IsFailingIn(cPlaceB));

    log.BeginVisit(backoff.IsFailingIn(cPlaceB));
    REQUIRE(log.ShouldLogError(0x1234));

    backoff.NoteSuccess();
    REQUIRE_FALSE(backoff.IsFailingIn(cPlaceA));
    log.BeginVisit(backoff.IsFailingIn(cPlaceA));
    REQUIRE(log.ShouldLogError(0x1234));
}
