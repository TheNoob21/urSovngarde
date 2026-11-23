#include <catch2/catch.hpp>

#include <Services/PlayerMapRules.h>

TEST_CASE("The world map gathers its markers again when it opens and when ours change", "[player_map]")
{
    using PlayerMapRules::RefreshDue;
    CHECK(RefreshDue(true, false, false));
    CHECK(RefreshDue(true, true, true));
    CHECK_FALSE(RefreshDue(true, true, false));
    CHECK_FALSE(RefreshDue(false, false, true));
    CHECK_FALSE(RefreshDue(false, true, true));
}

TEST_CASE("A gathering the game could not queue is tried again until it is", "[player_map]")
{
    using PlayerMapRules::RefreshDue;
    using PlayerMapRules::StillOwed;

    // The map opens, the first try fails, and nothing of ours moves after it.
    bool owed = false;
    bool due = RefreshDue(true, false, owed);
    REQUIRE(due);
    owed = StillOwed(due, false, owed);
    CHECK(owed);
    // The next look, the map still open: tried again.
    due = RefreshDue(true, true, owed);
    CHECK(due);
    owed = StillOwed(due, true, owed);
    CHECK_FALSE(owed);
    // Then nothing more until ours change.
    CHECK_FALSE(RefreshDue(true, true, owed));

    // A change while it is shut waits for it to open; not due while it is shut.
    CHECK(StillOwed(false, false, true));
    CHECK_FALSE(StillOwed(false, false, false));
}

TEST_CASE("A teammate's marker near this player stands off toward them, out of the game's discovery reach", "[player_map]")
{
    struct Vec
    {
        float x, y, z;
    };
    using PlayerMapRules::kKeepAwayDistance;
    using PlayerMapRules::MarkerSpot;
    const auto distance = [](const Vec& a, const Vec& b)
    {
        return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
    };

    const Vec player{100.f, 200.f, 50.f};

    // Far enough: on the teammate.
    const Vec farAway{100.f, 2200.f, 50.f};
    const Vec atFarAway = MarkerSpot(player, farAway);
    CHECK(atFarAway.x == farAway.x);
    CHECK(atFarAway.y == farAway.y);

    // Ten metres east and a little above: due east of the player, at the keep-away distance and the player's height.
    const Vec nearBy{800.f, 200.f, 120.f};
    const Vec atNearBy = MarkerSpot(player, nearBy);
    CHECK(atNearBy.x == Approx(100.f + kKeepAwayDistance));
    CHECK(atNearBy.y == Approx(200.f));
    CHECK(atNearBy.z == Approx(50.f));
    CHECK(distance(atNearBy, player) == Approx(kKeepAwayDistance));
    // Well beyond the game's iMapMarkerRevealDistance (1000).
    CHECK(distance(atNearBy, player) > 1000.f);

    // Straight above: to the north.
    const Vec above{100.f, 200.f, 400.f};
    const Vec atAbove = MarkerSpot(player, above);
    CHECK(atAbove.y == Approx(200.f + kKeepAwayDistance));
    CHECK(distance(atAbove, player) == Approx(kKeepAwayDistance));

    // A marker is placed again before the player gets within the game's reach.
    CHECK(PlayerMapRules::kPlaceAgainDistance > 1000.f);
    CHECK(PlayerMapRules::kPlaceAgainDistance < kKeepAwayDistance);
}
