#include <catch2/catch.hpp>

#include <limits>

#include <MoverNearness.h>

using namespace MoverNearness;

TEST_CASE("A player beside the object may move it", "[movernearness]")
{
    REQUIRE(IsNear(true, true, {0.f, 0.f, 0.f}, {100.f, 50.f, 0.f}));
    REQUIRE(IsNear(true, true, {0.f, 0.f, 0.f}, {kMaxDistance, 0.f, 0.f}));
}

TEST_CASE("A player too far from the object may not move it", "[movernearness]")
{
    REQUIRE_FALSE(IsNear(true, true, {0.f, 0.f, 0.f}, {kMaxDistance + 1.f, 0.f, 0.f}));
}

TEST_CASE("A player whose grid does not hold the object's cell may not move it", "[movernearness]")
{
    REQUIRE_FALSE(IsNear(false, true, {0.f, 0.f, 0.f}, {10.f, 0.f, 0.f}));
}

TEST_CASE("A player with no position yet may not move an object", "[movernearness]")
{
    REQUIRE_FALSE(IsNear(true, false, {0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}));
}

TEST_CASE("A place that is no number is not near", "[movernearness]")
{
    const float cNaN = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(IsNear(true, true, {0.f, 0.f, 0.f}, {cNaN, 0.f, 0.f}));
}

TEST_CASE("A dropped item may be moved anywhere in the sender's loaded grid, no farther", "[movernearness]")
{
    REQUIRE(IsNear(true, true, {0.f, 0.f, 0.f}, {10000.f, 10000.f, 0.f}, kLoadedGridReach));
    REQUIRE_FALSE(IsNear(true, true, {0.f, 0.f, 0.f}, {kLoadedGridReach + 1.f, 0.f, 0.f}, kLoadedGridReach));
}
