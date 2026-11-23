#include <catch2/catch.hpp>

#include <StringCacheLimits.h>

using namespace StringCacheLimits;

TEST_CASE("a new short name is taken while the cache has room", "[stringcachelimits]")
{
    CHECK(MayWant(0, 0, 10, false));
    CHECK(MayWant(kMaxEntries - 1, 0, 10, false));
    CHECK(MayWant(100, kMaxEntries - 101, 10, false));
}

TEST_CASE("a new name is refused once the cache and its waiting names fill it", "[stringcachelimits]")
{
    CHECK_FALSE(MayWant(kMaxEntries, 0, 10, false));
    CHECK_FALSE(MayWant(kMaxEntries - 5, 5, 10, false));
    CHECK_FALSE(MayWant(0, kMaxEntries, 10, false));
}

TEST_CASE("a name already in the cache is always fine", "[stringcachelimits]")
{
    CHECK(MayWant(kMaxEntries, 0, 10, true));
    CHECK(MayWant(kMaxEntries, kMaxEntries, kLongestName + 1, true));
}

TEST_CASE("a name over the length limit is not kept", "[stringcachelimits]")
{
    CHECK(MayWant(0, 0, kLongestName, false));
    CHECK_FALSE(MayWant(0, 0, kLongestName + 1, false));
}

TEST_CASE("the cache can never count past what its 16-bit ids hold", "[stringcachelimits]")
{
    static_assert(kMaxEntries < 0xFFFF);
}
