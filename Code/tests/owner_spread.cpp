#include <catch2/catch.hpp>

#include <OwnerSpread.h>

TEST_CASE("A leaving owner's NPCs go to the game running the fewest, within the best tier", "[owner_spread]")
{
    using OwnerSpread::Order;

    CHECK(Order({}).empty());
    // One tier, the first in connection order already runs 40: the second goes first.
    CHECK(Order({{0, 40}, {0, 3}}) == std::vector<size_t>{1, 0});
    // A better tier still wins, however many it runs.
    CHECK(Order({{0, 40}, {2, 0}}) == std::vector<size_t>{0, 1});
    CHECK(Order({{1, 5}, {0, 9}, {0, 2}, {1, 1}}) == std::vector<size_t>{2, 1, 3, 0});
    // Equal counts keep the order given.
    CHECK(Order({{0, 4}, {0, 4}, {0, 4}}) == std::vector<size_t>{0, 1, 2});
}

TEST_CASE("The leader's game takes an NPC near its player, or one no member is near", "[owner_spread]")
{
    using OwnerSpread::LeaderTakes;

    CHECK(LeaderTakes(100.f, 50.f));
    CHECK(LeaderTakes(OwnerSpread::kNear, 10.f));
    // Two cells away with a member standing next to it: the member's game keeps it.
    CHECK_FALSE(LeaderTakes(9000.f, 300.f));
    CHECK_FALSE(LeaderTakes(std::nullopt, 300.f));
    // Nobody else near: as before, the leader takes it.
    CHECK(LeaderTakes(9000.f, 7000.f));
    CHECK(LeaderTakes(9000.f, std::nullopt));
}

TEST_CASE("A member near an NPC takes it from the leader only once the leader is far", "[owner_spread]")
{
    using OwnerSpread::MemberTakesFromLeader;

    CHECK(MemberTakesFromLeader(9000.f, 500.f));
    CHECK(MemberTakesFromLeader(std::nullopt, 500.f));
    // Between near and far nothing moves, so an NPC at the edge does not go back and forth.
    CHECK_FALSE(MemberTakesFromLeader(7000.f, 500.f));
    CHECK_FALSE(MemberTakesFromLeader(OwnerSpread::kFar, 500.f));
    CHECK_FALSE(MemberTakesFromLeader(9000.f, 6500.f));
    CHECK_FALSE(MemberTakesFromLeader(9000.f, std::nullopt));
    // What the leader's game takes, a member's never takes back from it, and the other way round.
    for (const float cLeader : {1000.f, 6000.f, 7000.f, 8000.f, 9000.f})
        for (const float cMember : {500.f, 5000.f, 6000.f})
            CHECK_FALSE((MemberTakesFromLeader(cLeader, cMember) && OwnerSpread::IsNear(cLeader)));
}

TEST_CASE("Only a member whose game may take an NPC over counts as near it", "[owner_spread]")
{
    using OwnerSpread::kArrivalGrace;
    using OwnerSpread::MayTakeOver;
    using namespace std::chrono_literals;

    // Loaded as its own, settled, not declined: it may.
    CHECK(MayTakeOver(true, false, false, kArrivalGrace));
    CHECK(MayTakeOver(true, false, false, 5min));
    // Not loaded, only on a stand-in, declined, or just arrived: it may not.
    CHECK_FALSE(MayTakeOver(false, false, false, 5min));
    CHECK_FALSE(MayTakeOver(true, true, false, 5min));
    CHECK_FALSE(MayTakeOver(true, false, true, 5min));
    CHECK_FALSE(MayTakeOver(true, false, false, kArrivalGrace - 1s));

    // The nearest member declined the NPC; the next one, further off, has it loaded. The leader is two cells away.
    struct Member
    {
        float Distance;
        bool Eligible;
    };
    const Member cMembers[] = {{300.f, MayTakeOver(true, false, true, 5min)}, {2500.f, MayTakeOver(true, false, false, 5min)}};
    std::optional<float> nearest;
    for (const Member& cMember : cMembers)
        if (cMember.Eligible && (!nearest || cMember.Distance < *nearest))
            nearest = cMember.Distance;
    // The next eligible member takes it over, and the leader is not given it.
    REQUIRE(nearest);
    CHECK(*nearest == 2500.f);
    CHECK(OwnerSpread::MemberTakesFromLeader(9000.f, nearest));
    CHECK_FALSE(OwnerSpread::LeaderTakes(9000.f, nearest));
    // With nobody eligible near it, the leader is given it.
    CHECK(OwnerSpread::LeaderTakes(9000.f, std::nullopt));
}
