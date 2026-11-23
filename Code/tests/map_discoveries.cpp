#include <catch2/catch.hpp>

#include <MapDiscoveries.h>

#include <algorithm>

using MapDiscoveries::kCanTravel;
using MapDiscoveries::kVisible;

TEST_CASE("a map location one member discovers is the party's, and a member who joins later gets it", "[map_discoveries]")
{
    MapDiscoveries::Party party;
    const uint64_t cRiverwood = (0ull << 32) | 0x1D4F1;
    const uint64_t cBleakFalls = (0ull << 32) | 0x1D4F2;

    SECTION("a new marker is news once; a second member's game discovering it too is not")
    {
        CHECK(party.Add(cRiverwood, true));
        CHECK_FALSE(party.Add(cRiverwood, true));
        CHECK_FALSE(party.Add(cRiverwood, false));
    }

    SECTION("a marker only seen on the map becomes news again when a member may travel to it")
    {
        CHECK(party.Add(cBleakFalls, false));
        CHECK(party.Add(cBleakFalls, true));
        CHECK_FALSE(party.Add(cBleakFalls, false));
    }

    SECTION("the whole list goes to a joining member, each marker with what may be done with it")
    {
        party.Add(cRiverwood, true);
        party.Add(cBleakFalls, false);
        auto all = party.All();
        std::sort(all.begin(), all.end());
        CHECK(all == std::vector<std::pair<uint64_t, bool>>{{cRiverwood, true}, {cBleakFalls, false}});
    }
}

TEST_CASE("this game sends only what its map discovered since it last looked", "[map_discoveries]")
{
    MapDiscoveries::Watch watch;

    SECTION("the first look learns the save's map and sends none of it")
    {
        CHECK(watch.Look({{0x100, kVisible | kCanTravel}, {0x101, 0}}).empty());
    }

    SECTION("a marker that comes on the map, or becomes a destination, is found")
    {
        CHECK(watch.Look({{0x100, 0}, {0x101, kVisible}, {0x102, kVisible | kCanTravel}}).empty());
        const auto cFound = watch.Look({{0x100, kVisible}, {0x101, kVisible | kCanTravel}, {0x102, kVisible | kCanTravel}});
        CHECK(cFound == std::vector<MapDiscoveries::Watch::Found>{{0x100, false}, {0x101, true}});
        CHECK(watch.Look({{0x100, kVisible}, {0x101, kVisible | kCanTravel}}).empty());
    }

    SECTION("a marker taken off the map by an older save is not news, and coming back is")
    {
        CHECK(watch.Look({{0x100, kVisible}}).empty());
        CHECK(watch.Look({{0x100, 0}}).empty());
        CHECK(watch.Look({{0x100, kVisible}}).size() == 1);
    }

    SECTION("a discovery of the party's put on the map here is not sent back")
    {
        CHECK(watch.Look({{0x100, 0}}).empty());
        watch.Learned(0x100, true);
        CHECK(watch.Look({{0x100, kVisible | kCanTravel}}).empty());
    }

    SECTION("a party discovery for a marker not looked at yet leaves it to the first look")
    {
        watch.Learned(0x200, false);
        CHECK(watch.Look({{0x200, kVisible | kCanTravel}}).empty());
    }

    SECTION("after a load the save's map is learned again")
    {
        CHECK(watch.Look({{0x100, 0}}).empty());
        watch.Reset();
        CHECK(watch.Look({{0x100, kVisible}}).empty());
    }
}

TEST_CASE("a party discovery is put on this map only when it is not there yet", "[map_discoveries]")
{
    CHECK(MapDiscoveries::NeedsAdding(0, false));
    CHECK(MapDiscoveries::NeedsAdding(0, true));
    CHECK_FALSE(MapDiscoveries::NeedsAdding(kVisible, false));
    CHECK(MapDiscoveries::NeedsAdding(kVisible, true));
    CHECK_FALSE(MapDiscoveries::NeedsAdding(kVisible | kCanTravel, true));
    CHECK_FALSE(MapDiscoveries::NeedsAdding(kVisible | kCanTravel, false));
}
