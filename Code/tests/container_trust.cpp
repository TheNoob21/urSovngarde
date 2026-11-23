#include <catch2/catch.hpp>

#include <limits>

#include <ContainerTrust.h>

using namespace ContainerTrust;

TEST_CASE("A player with the container in range changes it by an honest count", "[containertrust]")
{
    CHECK(MayChange(true, 1));
    CHECK(MayChange(true, -1));
    CHECK(MayChange(true, 5000));
    CHECK(MayChange(true, -5000));
}

TEST_CASE("A player whose grid does not hold the container may not change it", "[containertrust]")
{
    CHECK_FALSE(MayChange(false, 1));
    CHECK_FALSE(MayChange(false, -1));
}

TEST_CASE("A change of none is no change", "[containertrust]")
{
    CHECK_FALSE(MayChange(true, 0));
}

TEST_CASE("The largest honest count is let through and one more is not", "[containertrust]")
{
    CHECK(MayChange(true, static_cast<int32_t>(kMaxChange)));
    CHECK(MayChange(true, -static_cast<int32_t>(kMaxChange)));
    CHECK_FALSE(MayChange(true, static_cast<int32_t>(kMaxChange) + 1));
    CHECK_FALSE(MayChange(true, -static_cast<int32_t>(kMaxChange) - 1));
}

TEST_CASE("Counts near the limits of a 32-bit number are refused", "[containertrust]")
{
    CHECK_FALSE(MayChange(true, std::numeric_limits<int32_t>::max()));
    CHECK_FALSE(MayChange(true, std::numeric_limits<int32_t>::min()));
    CHECK_FALSE(MayChange(true, std::numeric_limits<int32_t>::min() + 1));
}
