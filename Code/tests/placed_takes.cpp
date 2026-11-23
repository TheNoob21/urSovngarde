#include <catch2/catch.hpp>

#include <PlacedTakes.h>

TEST_CASE("a placed item a player takes stays taken for the others, a quest object stays for each player", "[placed_takes]")
{
    SECTION("a sword taken from a table is told to the server, whatever kind of item it is")
    {
        CHECK(PlacedTakes::ShouldShare(false, false, false));
    }

    SECTION("a dropped item is DroppedItemService's, not a placed one")
    {
        CHECK_FALSE(PlacedTakes::ShouldShare(true, false, false));
    }

    SECTION("a quest object is left for a player who comes later: their quest needs their own")
    {
        CHECK_FALSE(PlacedTakes::ShouldShare(false, true, false));
    }

    SECTION("this game's copy of the other player taking it is that player's to tell")
    {
        CHECK_FALSE(PlacedTakes::ShouldShare(false, false, true));
    }
}
