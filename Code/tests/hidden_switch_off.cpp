#include <catch2/catch.hpp>

#include <Games/HiddenSwitchOff.h>

using HiddenSwitchOff::IsForeign;

TEST_CASE("a switch-off of a reference hidden here for another player's copy is told apart", "[followers]")
{
    SECTION("someone else switching off a reference already hidden here is caught")
    {
        CHECK(IsForeign(true, true, false));
    }

    SECTION("this game's own hide lands on a reference not yet switched off, so it is not caught")
    {
        CHECK_FALSE(IsForeign(true, false, false));
    }

    SECTION("a model reload's switch-off is this game's own")
    {
        CHECK_FALSE(IsForeign(true, true, true));
    }

    SECTION("a reference not hidden here is none of its business")
    {
        CHECK_FALSE(IsForeign(false, true, false));
        CHECK_FALSE(IsForeign(false, false, false));
    }
}
