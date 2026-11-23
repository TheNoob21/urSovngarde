#include <catch2/catch.hpp>

#include <Services/FollowMenuOk.h>

using FollowMenuOk::Action;
using FollowMenuOk::Decide;

TEST_CASE("the follow menu's OK follows the leader instead of waiting", "[party][sleep]")
{
    SECTION("a menu opened to follow the leader's fast travel follows, whatever the vote's state")
    {
        CHECK(Decide(true, true, true, true) == Action::kFollow);
        CHECK(Decide(true, false, true, true) == Action::kFollow);
        CHECK(Decide(true, true, false, true) == Action::kFollow);
    }

    SECTION("offline, the game's own OK, even with a follow left over")
    {
        CHECK(Decide(false, true, true, true) == Action::kGameOk);
        CHECK(Decide(false, true, true, false) == Action::kGameOk);
    }

    SECTION("an ordinary Sleep/Wait menu online is a vote once the main loop presses the agreed OK")
    {
        CHECK(Decide(true, true, true, false) == Action::kVote);
        CHECK(Decide(true, false, true, false) == Action::kGameOk);
        CHECK(Decide(true, true, false, false) == Action::kGameOk);
    }
}
