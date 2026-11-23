#include <catch2/catch.hpp>

#include <bot/BotRules.h>

TEST_CASE("the bot's additem ref target is a living current NPC it owns or a party member's follower", "[bot]")
{
    REQUIRE(BotRules::MaySendInventoryToRef(false, false, 2, false, 1, true));
    REQUIRE(BotRules::MaySendInventoryToRef(false, false, 2, true, 0, false));
    REQUIRE_FALSE(BotRules::MaySendInventoryToRef(true, false, 2, false, 1, true));
    REQUIRE_FALSE(BotRules::MaySendInventoryToRef(false, true, 2, false, 1, true));
    REQUIRE_FALSE(BotRules::MaySendInventoryToRef(false, false, 0, false, 1, true));
    REQUIRE_FALSE(BotRules::MaySendInventoryToRef(false, false, 2, false, 0, true));
    REQUIRE_FALSE(BotRules::MaySendInventoryToRef(false, false, 2, false, 1, false));
}
