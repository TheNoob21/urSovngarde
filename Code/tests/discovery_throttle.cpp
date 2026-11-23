#include <catch2/catch.hpp>

#include <Services/DiscoveryThrottle.h>

using Clock = DiscoveryThrottle::Clock;
using namespace std::chrono_literals;

TEST_CASE("Discovery runs every frame while connected", "[discovery]")
{
    DiscoveryThrottle throttle;
    const auto t0 = Clock::now();
    for (int i = 0; i < 100; ++i)
        REQUIRE(throttle.ShouldRun(true, true, false, t0 + i * 16ms));
    // Even with no cell loaded: behaviour once connected is unchanged.
    REQUIRE(throttle.ShouldRun(true, false, false, t0 + 2s));
}

TEST_CASE("Discovery is skipped at the main menu while disconnected", "[discovery]")
{
    DiscoveryThrottle throttle;
    const auto t0 = Clock::now();
    for (int i = 0; i < 200; ++i)
        REQUIRE_FALSE(throttle.ShouldRun(false, false, false, t0 + i * 16ms));
}

TEST_CASE("Discovery runs a few times a second in game while disconnected", "[discovery]")
{
    DiscoveryThrottle throttle;
    const auto t0 = Clock::now();
    int runs = 0;
    for (int i = 0; i < 60 * 4; ++i) // four seconds at 60 fps
        runs += throttle.ShouldRun(false, true, false, t0 + i * 16ms) ? 1 : 0;
    REQUIRE(runs >= 14);
    REQUIRE(runs <= 17);
}

TEST_CASE("A cell loading after the menu is picked up within the interval", "[discovery]")
{
    DiscoveryThrottle throttle;
    const auto t0 = Clock::now();
    REQUIRE_FALSE(throttle.ShouldRun(false, false, false, t0));
    REQUIRE_FALSE(throttle.ShouldRun(false, true, false, t0 + 100ms));
    REQUIRE(throttle.ShouldRun(false, true, false, t0 + 260ms));
}

TEST_CASE("No cell but tracked actors still runs, paced, until they are gone", "[discovery]")
{
    DiscoveryThrottle throttle;
    const auto t0 = Clock::now();
    REQUIRE(throttle.ShouldRun(false, false, true, t0));
    REQUIRE_FALSE(throttle.ShouldRun(false, false, true, t0 + 16ms));
    REQUIRE(throttle.ShouldRun(false, false, true, t0 + 260ms));
    // Nothing tracked any more: skipped again.
    REQUIRE_FALSE(throttle.ShouldRun(false, false, false, t0 + 600ms));
}
