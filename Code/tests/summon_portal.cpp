#include <catch2/catch.hpp>

#include <SummonPortal.h>

using SummonPortal::kWindowMs;

TEST_CASE("A summon's copy appearing soon after its cast plays its portal", "[summon_portal]")
{
    SummonPortal::Queue queue;
    queue.Note(0xA1, 1000);
    REQUIRE(queue.Take(1000 + 1500) == 0xA1);
    // Used up: a second creature appearing then plays none.
    REQUIRE(queue.Take(1000 + 1600) == 0);
}

TEST_CASE("A summon only come near, long after its cast, plays no portal", "[summon_portal]")
{
    SummonPortal::Queue queue;
    REQUIRE(queue.Take(500) == 0);
    queue.Note(0xA1, 1000);
    REQUIRE(queue.Take(1000 + kWindowMs) == 0xA1);
    queue.Note(0xA2, 1000);
    REQUIRE(queue.Take(1000 + kWindowMs + 1) == 0);
    // The stale one was dropped, not kept for later.
    REQUIRE(queue.Count == 0);
}

TEST_CASE("Summons cast at once play their portals oldest first", "[summon_portal]")
{
    SummonPortal::Queue queue;
    queue.Note(0xA1, 1000);
    queue.Note(0xA2, 1200);
    REQUIRE(queue.Take(1300) == 0xA1);
    REQUIRE(queue.Take(1400) == 0xA2);
    REQUIRE(queue.Take(1500) == 0);
}

TEST_CASE("A summon with no portal art is not kept, and the oldest goes when the queue is full", "[summon_portal]")
{
    SummonPortal::Queue queue;
    queue.Note(0, 1000);
    REQUIRE(queue.Count == 0);
    for (uint64_t i = 1; i <= SummonPortal::kKept + 1; ++i)
        queue.Note(i, 1000 + i);
    REQUIRE(queue.Count == SummonPortal::kKept);
    REQUIRE(queue.Take(1100) == 2);
}

TEST_CASE("A clock that stepped back keeps no portal", "[summon_portal]")
{
    SummonPortal::Queue queue;
    queue.Note(0xA1, 5000);
    REQUIRE(queue.Take(4000) == 0);
}
