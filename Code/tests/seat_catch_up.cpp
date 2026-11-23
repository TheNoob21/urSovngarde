#include <catch2/catch.hpp>

#include <SeatCatchUp.h>

TEST_CASE("a player sent a seated NPC later is told its seat at once, if it is current", "[npc_furniture]")
{
    using NpcFurniture::kExpireMs;
    using NpcFurniture::kStateNone;
    using NpcFurniture::kStateSitting;
    using NpcFurniture::kStateSleeping;
    REQUIRE(SeatCatchUp::Tell(kStateSitting, 4, 4, true, 1000));
    REQUIRE(SeatCatchUp::Tell(kStateSleeping, 4, 4, true, 0));
    // Up again, no furniture, another owner since, epoch 0, or too old for a copy to keep.
    REQUIRE_FALSE(SeatCatchUp::Tell(kStateNone, 4, 4, true, 1000));
    REQUIRE_FALSE(SeatCatchUp::Tell(kStateSitting, 4, 4, false, 1000));
    REQUIRE_FALSE(SeatCatchUp::Tell(kStateSitting, 4, 5, true, 1000));
    REQUIRE_FALSE(SeatCatchUp::Tell(kStateSitting, 0, 0, true, 1000));
    REQUIRE_FALSE(SeatCatchUp::Tell(kStateSitting, 4, 4, true, kExpireMs));
    // A way in or out is the owner's own and never sent.
    REQUIRE_FALSE(SeatCatchUp::Tell(2, 4, 4, true, 1000));
}
