#include <catch2/catch.hpp>
#include <Services/ExtensionAreas.h>
#include <map>
#include <set>

namespace
{
struct State
{
    uint32_t Owner{};
    std::set<uint32_t> Delivered;
};
using Areas = std::map<std::pair<uint32_t, uint64_t>, State>;
constexpr std::pair<uint32_t, uint64_t> kParked{7, 0x161}, kOther{7, 0x162};

// As RelayArea replays a kept state: to each viewer in range it has not reached yet.
bool Replays(const State& acState, uint32_t aViewer)
{
    return !acState.Delivered.contains(aViewer);
}
} // namespace

TEST_CASE("A state left in the world outlives its owner and reaches whoever reconnects or joins", "[server.extension]")
{
    // Player 1 parks a placed actor; players 1 and 2 have it.
    Areas areas;
    areas[kParked] = {1, {1, 2}};
    areas[kOther] = {2, {1, 2}};

    ExtensionAreas::Leave(areas, 1);
    REQUIRE(areas.size() == 2);
    CHECK(areas[kParked].Owner == 0);
    CHECK(areas[kParked].Delivered == std::set<uint32_t>{2});
    CHECK(areas[kOther].Owner == 2);
    CHECK_FALSE(Replays(areas[kParked], 2));

    // Player 2 drops and comes back under a new id; a third player joins late.
    ExtensionAreas::Leave(areas, 2);
    REQUIRE(areas.size() == 2);
    CHECK(areas[kOther].Owner == 0);
    for (const uint32_t cViewer : {3u, 4u})
    {
        CHECK(Replays(areas[kParked], cViewer));
        CHECK(Replays(areas[kOther], cViewer));
    }
}

TEST_CASE("A state without an owner goes to whoever writes it next, within their room", "[server.extension]")
{
    CHECK(ExtensionAreas::NextOwner(0, 3, 0) == 3);
    CHECK(ExtensionAreas::NextOwner(0, 3, ExtensionAreas::kMaxPerOwner) == 0);
    CHECK(ExtensionAreas::NextOwner(1, 3, ExtensionAreas::kMaxPerOwner - 1) == 3);
    CHECK(ExtensionAreas::NextOwner(1, 3, ExtensionAreas::kMaxPerOwner) == 1);
    CHECK(ExtensionAreas::NextOwner(3, 3, ExtensionAreas::kMaxPerOwner) == 3);
}

TEST_CASE("An add-on at its limit drops only a state with no owner that no one has", "[server.extension]")
{
    Areas areas;
    areas[kParked] = {0, {2}};
    areas[kOther] = {1, {}};
    areas[{8, 0x163}] = {0, {}};
    CHECK_FALSE(ExtensionAreas::EvictUnowned(areas, 7));
    CHECK(areas.size() == 3);

    areas[kParked].Delivered.clear();
    REQUIRE(ExtensionAreas::EvictUnowned(areas, 7));
    CHECK_FALSE(areas.contains(kParked));
    CHECK(areas.contains(kOther));
    CHECK(areas.contains({8, 0x163}));
}

TEST_CASE("Area states are bounded for each player, each add-on and the whole server", "[server.extension]")
{
    using namespace ExtensionAreas;
    Areas areas;
    auto fill = [&](uint32_t aAddon, uint32_t aOwner, size_t aCount)
    {
        for (size_t i = 0; i < aCount; ++i)
        {
            REQUIRE(MakeRoom(areas, aAddon, aOwner));
            areas[{aAddon, areas.size() + 1}] = {aOwner, {}};
        }
    };

    // A player at its limit is refused; once it leaves, its states stay and count for no one.
    fill(1, 10, kMaxPerOwner);
    CHECK_FALSE(MakeRoom(areas, 1, 10));
    Leave(areas, 10);
    CHECK(Owned(areas, 10) == 0);
    CHECK(MakeRoom(areas, 1, 11));

    // A client that leaves and comes back under new ids fills one add-on; it then drops its own unowned states.
    for (uint32_t player = 11; areas.size() < kMaxPerAddon; ++player)
    {
        fill(1, player, std::min(kMaxPerOwner, kMaxPerAddon - areas.size()));
        Leave(areas, player);
    }
    REQUIRE(MakeRoom(areas, 1, 50));
    CHECK(areas.size() == kMaxPerAddon - 1);

    // The same under fresh add-on ids: the store and the number of add-ons stay bounded.
    uint32_t player = 100;
    for (uint32_t addon = 2; addon < 200; ++addon, ++player)
    {
        for (size_t i = 0; i < 64; ++i)
            if (MakeRoom(areas, addon, player))
                areas[{addon, 0x10000 + areas.size()}] = {player, {}};
        Leave(areas, player);
    }
    std::set<uint32_t> addons;
    for (const auto& cEntry : areas)
        addons.insert(cEntry.first.first);
    CHECK(areas.size() <= kMaxTotal);
    CHECK(addons.size() <= kMaxAddons);
}

TEST_CASE("States someone owns or has are never dropped to make room", "[server.extension]")
{
    using namespace ExtensionAreas;
    Areas areas;
    for (uint32_t addon = 1; addon <= kMaxAddons; ++addon)
        areas[{addon, 1}] = {0, {7}};
    CHECK_FALSE(MakeRoom(areas, kMaxAddons + 1, 9));
    CHECK(areas.size() == kMaxAddons);
    CHECK(MakeRoom(areas, 1, 9));

    areas[{1, 1}].Delivered.clear();
    CHECK(MakeRoom(areas, kMaxAddons + 1, 9));
    CHECK_FALSE(areas.contains({1, 1}));
}

TEST_CASE("A full server drops a state no one owns or has, of any add-on", "[server.extension]")
{
    using namespace ExtensionAreas;
    Areas areas;
    for (uint32_t addon = 1; addon <= kMaxAddons; ++addon)
        for (uint64_t reference = 1; reference <= kMaxTotal / kMaxAddons; ++reference)
            areas[{addon, reference}] = {0, {}};
    REQUIRE(areas.size() == kMaxTotal);
    // An add-on that already holds states, under its own limit, still gets room from another one.
    areas[{5, 1}] = {9, {}};
    REQUIRE(MakeRoom(areas, 5, 9));
    CHECK(areas.size() == kMaxTotal - 1);
    CHECK(areas.contains({5, 1}));

    // With every state owned or seen, nothing is dropped and the new state is refused.
    for (auto& entry : areas)
        entry.second.Delivered = {2};
    areas[{1, 1000}] = {0, {2}};
    CHECK_FALSE(MakeRoom(areas, 5, 9));
    CHECK(areas.size() == kMaxTotal);
}

TEST_CASE("Too many add-ons with states make room from the smallest one no one has", "[server.extension]")
{
    using namespace ExtensionAreas;
    Areas areas;
    for (uint32_t addon = 1; addon <= kMaxAddons; ++addon)
        for (uint64_t reference = 1; reference <= (addon == 1 ? 50u : addon == 7 ? 2u : 5u); ++reference)
            areas[{addon, reference}] = {0, {}};
    REQUIRE(MakeRoom(areas, kMaxAddons + 1, 9));
    CHECK_FALSE(areas.contains({7, 1}));
    CHECK(areas.contains({1, 50}));
}
