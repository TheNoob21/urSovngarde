#include <catch2/catch.hpp>

#include <limits>

#include <Services/Generic/ContainerLidRules.h>

using namespace ContainerLidRules;
using namespace std::chrono_literals;

TEST_CASE("A lid stays open while its user stands at the chest", "[containerlid]")
{
    REQUIRE_FALSE(ShouldClose(true, 80.f, 10s));
    REQUIRE_FALSE(ShouldClose(true, kLeftAt, 2min));
}

TEST_CASE("A lid closes once its user walks away", "[containerlid]")
{
    REQUIRE(ShouldClose(true, kLeftAt + 1.f, 1s));
}

TEST_CASE("A lid closes when its user is gone", "[containerlid]")
{
    REQUIRE(ShouldClose(false, 0.f, 1s));
}

TEST_CASE("A lid closes after a long while whatever its user does", "[containerlid]")
{
    REQUIRE(ShouldClose(true, 10.f, kOpenAtMost));
}

TEST_CASE("A distance that is no number closes the lid", "[containerlid]")
{
    REQUIRE(ShouldClose(true, std::numeric_limits<float>::quiet_NaN(), 1s));
}
