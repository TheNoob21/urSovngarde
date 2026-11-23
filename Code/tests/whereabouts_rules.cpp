#include <catch2/catch.hpp>

#include <WhereaboutsRules.h>

using namespace WhereaboutsRules;

TEST_CASE("whereabouts go out four times a second while something moves and every two seconds otherwise", "[whereabouts]")
{
    // Something moved: not before a quarter of a second has passed, then at once.
    CHECK_FALSE(SendDue(0, true));
    CHECK_FALSE(SendDue(kMovingEveryMs - 1, true));
    CHECK(SendDue(kMovingEveryMs, true));
    CHECK(SendDue(kMovingEveryMs + 100, true));

    // Nothing moved: only the heartbeat.
    CHECK_FALSE(SendDue(kMovingEveryMs, false));
    CHECK_FALSE(SendDue(kHeartbeatMs - 1, false));
    CHECK(SendDue(kHeartbeatMs, false));
    CHECK(SendDue(kHeartbeatMs * 3, false));

    // The icon is told at least every 250 ms while the map is open, and the idle cost is the old one message in two seconds.
    CHECK(kMovingEveryMs <= 250);
    CHECK(kHeartbeatMs == 2000);
}

TEST_CASE("a map marker waits for a small step while the world map is open and a big one otherwise", "[whereabouts]")
{
    CHECK(FollowDistance(false) == kFollowDistance);
    CHECK(FollowDistance(true) == kFollowDistanceMapOpen);
    CHECK(kFollowDistanceMapOpen < kFollowDistance);
}
