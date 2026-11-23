#include <catch2/catch.hpp>

#include <ServerIdIndex.h>

#include <cstdint>

using ServerIdIndex::Kind;

namespace
{
// Entities stand in as plain numbers.
using Index = ServerIdIndex::Index<uint32_t>;
} // namespace

TEST_CASE("an entity is found by the server id its component carries", "[server-id-index]")
{
    Index index;
    index.Set(Kind::kRemote, 7, 0x100);
    index.Set(Kind::kObject, 9, 0x200);

    REQUIRE(index.Find(0x100) == 7u);
    REQUIRE(index.Find(0x200) == 9u);
    REQUIRE_FALSE(index.Find(0x300));
    REQUIRE(index.Find(Kind::kRemote, 0x100) == 7u);
    REQUIRE_FALSE(index.Find(Kind::kLocal, 0x100));
    REQUIRE(index.Size() == 2);
}

TEST_CASE("an actor this game runs comes before a copy, and a copy before an object", "[server-id-index]")
{
    Index index;
    index.Set(Kind::kObject, 3, 0x100);
    index.Set(Kind::kRemote, 2, 0x100);
    REQUIRE(index.Find(0x100) == 2u);

    index.Set(Kind::kLocal, 1, 0x100);
    REQUIRE(index.Find(0x100) == 1u);
    REQUIRE(index.Find(Kind::kObject, 0x100) == 3u);

    index.Erase(Kind::kLocal, 1);
    REQUIRE(index.Find(0x100) == 2u);
    index.Erase(Kind::kRemote, 2);
    REQUIRE(index.Find(0x100) == 3u);
}

TEST_CASE("a changed server id is found under the new one only", "[server-id-index]")
{
    Index index;
    index.Set(Kind::kObject, 4, 0x100);

    SECTION("the server gave the object a new id")
    {
        index.Set(Kind::kObject, 4, 0x180);
        REQUIRE_FALSE(index.Find(0x100));
        REQUIRE(index.Find(0x180) == 4u);
        REQUIRE(index.Size() == 1);
    }

    SECTION("set again to the same id, nothing changes")
    {
        index.Set(Kind::kObject, 4, 0x100);
        REQUIRE(index.Find(0x100) == 4u);
        index.Erase(Kind::kObject, 4);
        REQUIRE_FALSE(index.Find(0x100));
    }

    SECTION("an actor handed to this game keeps its id under the other kind")
    {
        index.Set(Kind::kRemote, 5, 0x300);
        index.Erase(Kind::kRemote, 5);
        index.Set(Kind::kLocal, 5, 0x300);
        REQUIRE(index.Find(0x300) == 5u);
        REQUIRE_FALSE(index.Find(Kind::kRemote, 0x300));
    }
}

TEST_CASE("an entity that goes is no longer found", "[server-id-index]")
{
    Index index;
    index.Set(Kind::kRemote, 6, 0x100);
    index.Set(Kind::kRemote, 8, 0x200);

    SECTION("its component removed")
    {
        index.Erase(Kind::kRemote, 6);
        REQUIRE_FALSE(index.Find(0x100));
        REQUIRE(index.Find(0x200) == 8u);
        REQUIRE(index.Size() == 1);
    }

    SECTION("a component of another kind it never had changes nothing")
    {
        index.Erase(Kind::kLocal, 6);
        REQUIRE(index.Find(0x100) == 6u);
    }

    SECTION("two entities with one id: the other stays when one goes")
    {
        index.Set(Kind::kRemote, 9, 0x100);
        REQUIRE(index.Find(0x100) == 6u);
        index.Erase(Kind::kRemote, 6);
        REQUIRE(index.Find(0x100) == 9u);
    }

    SECTION("all go at once")
    {
        index.Clear();
        REQUIRE_FALSE(index.Find(0x100));
        REQUIRE_FALSE(index.Find(0x200));
        REQUIRE(index.Size() == 0);
        index.Set(Kind::kRemote, 6, 0x100);
        REQUIRE(index.Find(0x100) == 6u);
    }
}
