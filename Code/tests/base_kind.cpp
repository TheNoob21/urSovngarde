#include <catch2/catch.hpp>

#include <BaseKind.h>

#include <utility>

TEST_CASE("two creatures with one name and different races are another kind", "[basekind]")
{
    const auto cVerdict = BaseKind::Decide(true, 0x1A, 0x1B, false);
    REQUIRE(cVerdict.Foreign);
    REQUIRE(cVerdict.By == BaseKind::Decider::Race);
}

TEST_CASE("one name and one race are the same kind", "[basekind]")
{
    REQUIRE_FALSE(BaseKind::Decide(true, 0x1A, 0x1A, false).Foreign);
}

TEST_CASE("another name on one race is the same kind", "[basekind]")
{
    REQUIRE_FALSE(BaseKind::Decide(false, 0x1A, 0x1A, false).Foreign);
}

TEST_CASE("another name on another non-person race is another kind", "[basekind]")
{
    REQUIRE(BaseKind::Decide(false, 0x1A, 0x1B, false).Foreign);
}

TEST_CASE("two people are never another kind", "[basekind]")
{
    REQUIRE_FALSE(BaseKind::Decide(false, 0x1A, 0x1B, true).Foreign);
    REQUIRE_FALSE(BaseKind::Decide(true, 0x1A, 0x1B, true).Foreign);
}

TEST_CASE("the name decides only when a race cannot be read", "[basekind]")
{
    REQUIRE_FALSE(BaseKind::Decide(true, 0, 0x1B, false).Foreign);
    REQUIRE_FALSE(BaseKind::Decide(true, 0x1A, 0, false).Foreign);
    const auto cOther = BaseKind::Decide(false, 0, 0x1B, false);
    REQUIRE(cOther.Foreign);
    REQUIRE(cOther.By == BaseKind::Decider::RaceUnreadable);
}
