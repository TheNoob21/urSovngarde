#include <catch2/catch.hpp>

#include <PartyRules.h>

TEST_CASE("A joining player is put in the largest party, the oldest on a tie", "[party_rules]")
{
    using PartyRules::PartyToJoin;

    CHECK_FALSE(PartyToJoin({}).has_value());
    CHECK(PartyToJoin({{4, 1}}) == 4u);
    // Somebody left the first party and made their own: the joiner still lands in one, the bigger.
    CHECK(PartyToJoin({{0, 2}, {1, 1}}) == 0u);
    CHECK(PartyToJoin({{0, 1}, {1, 3}}) == 1u);
    // Equal sizes: the oldest, whatever order the map gives them in.
    CHECK(PartyToJoin({{7, 2}, {3, 2}, {5, 2}}) == 3u);
}

TEST_CASE("A leader kicks only a member of its own party", "[party_rules]")
{
    using PartyRules::MayKick;

    CHECK(MayKick(1u, true, 1u));
    // Leader of party 1 kicking a member of party 2.
    CHECK_FALSE(MayKick(1u, true, 2u));
    CHECK_FALSE(MayKick(1u, true, std::nullopt));
    CHECK_FALSE(MayKick(1u, false, 1u));
    CHECK_FALSE(MayKick(std::nullopt, true, std::nullopt));
}
