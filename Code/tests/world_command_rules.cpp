#include <catch2/catch.hpp>
#include <WorldCommandRules.h>

TEST_CASE("time commands follow authenticated authority for both public listings", "[world.command]")
{
    for (const bool upstream : {false, true})
        for (const bool hub : {false, true})
        {
            CAPTURE(upstream, hub);
            const bool publicServer = upstream || hub;
            REQUIRE(WorldCommandRules::CanSetTime(true, false, publicServer));
            REQUIRE(WorldCommandRules::CanSetTime(true, true, publicServer));
            REQUIRE_FALSE(WorldCommandRules::CanSetTime(false, false, publicServer));
            REQUIRE(WorldCommandRules::CanSetTime(false, true, publicServer) == !publicServer);
        }
}
