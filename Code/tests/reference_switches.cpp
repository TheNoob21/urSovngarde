#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

// The encoding structs name TiltedCore's types without the namespace, as Message.h does for the message tests.
using namespace TiltedPhoques;

#include <ReferenceSwitches.h>

#include <vector>

namespace
{
std::vector<std::pair<uint32_t, bool>> All(const ReferenceSwitches& acSwitches)
{
    std::vector<std::pair<uint32_t, bool>> all;
    acSwitches.ForEach([&all](const GameId& acId, const bool aEnabled) { all.emplace_back(acId.BaseId, aEnabled); });
    return all;
}
} // namespace

TEST_CASE("A party keeps the latest switch of each reference", "[reference_switches]")
{
    ReferenceSwitches switches;
    CHECK(switches.Empty());
    CHECK_FALSE(switches.Find(GameId(0, 1)).has_value());

    switches.Set(GameId(0, 1), false);
    switches.Set(GameId(0, 2), true);
    switches.Set(GameId(0, 1), true);
    CHECK(switches.Size() == 2);
    CHECK(switches.Find(GameId(0, 1)) == true);
    CHECK(switches.Find(GameId(0, 2)) == true);
    // Oldest change first.
    CHECK(All(switches) == std::vector<std::pair<uint32_t, bool>>{{2, true}, {1, true}});
}

TEST_CASE("A long session's switches are capped, the longest unchanged going first", "[reference_switches]")
{
    ReferenceSwitches switches(3);
    switches.Set(GameId(0, 1), false);
    switches.Set(GameId(0, 2), false);
    switches.Set(GameId(0, 3), false);
    // Reference 1 switched again: now the newest.
    switches.Set(GameId(0, 1), true);
    switches.Set(GameId(0, 4), false);

    CHECK(switches.Size() == 3);
    CHECK_FALSE(switches.Find(GameId(0, 2)).has_value());
    CHECK(switches.Find(GameId(0, 1)) == true);
    CHECK(All(switches) == std::vector<std::pair<uint32_t, bool>>{{3, false}, {1, true}, {4, false}});

    // One reference switched over and over neither grows the list nor pushes the others out.
    for (int i = 0; i < 100; ++i)
        switches.Set(GameId(0, 4), i % 2 == 0);
    CHECK(switches.Size() == 3);
    CHECK(switches.Find(GameId(0, 3)) == false);
    CHECK(All(switches).size() == 3);

    CHECK(ReferenceSwitches::kMax == 16384);
}
