// The one exception table the launcher gives Wine for the game and itself (immersive_launcher/loader/SehTableMerge.h).

#include <catch2/catch.hpp>

#include <immersive_launcher/loader/SehTableMerge.h>

#include <cstdint>
#include <utility>
#include <vector>

using seh_table::Plan;
using seh_table::PlanMerge;
using seh_table::Result;
using seh_table::RuntimeFunction;

namespace
{
// Laid out like Skyrim VR in the launcher: game code, the game's table, the free rest of the load buffer,
// then the launcher's table and code.
constexpr uint32_t cGameCodeRva = 0x10D0;
constexpr uint32_t cGameTableRva = 0x36F4000;
constexpr uint32_t cFreeBegin = 0x3959000;
constexpr uint32_t cFreeEnd = 0x5000000;
constexpr uint32_t cLauncherTableRva = 0x5102000;
constexpr uint32_t cLauncherCodeRva = 0x55F3030;

// aCount functions of 0x40 bytes with 0x10 between them, each with its own unwind RVA.
std::vector<RuntimeFunction> MakeTable(uint32_t aFirstRva, uint32_t aCount, uint32_t aUnwindRva)
{
    std::vector<RuntimeFunction> table;
    for (uint32_t i = 0; i < aCount; ++i)
        table.push_back({aFirstRva + i * 0x50, aFirstRva + i * 0x50 + 0x40, aUnwindRva + i * 8});
    return table;
}

Result PlanFor(const std::vector<RuntimeFunction>& acGame, const std::vector<RuntimeFunction>& acLauncher, Plan& aPlan, uint32_t aBegin = cFreeBegin, uint32_t aEnd = cFreeEnd)
{
    return PlanMerge(acGame.data(), acGame.size(), cGameTableRva, acLauncher.data(), acLauncher.size(), cLauncherTableRva, aBegin, aEnd, aPlan);
}

// The binary search the system runs over an exception directory.
const RuntimeFunction* Find(const std::vector<RuntimeFunction>& acTable, uint32_t aRva)
{
    size_t low = 0;
    size_t high = acTable.size();
    while (low < high)
    {
        const size_t cMiddle = low + (high - low) / 2;
        if (aRva < acTable[cMiddle].BeginAddress)
            high = cMiddle;
        else if (aRva >= acTable[cMiddle].EndAddress)
            low = cMiddle + 1;
        else
            return &acTable[cMiddle];
    }
    return nullptr;
}
} // namespace

TEST_CASE("the merged exception table finds the game's functions and the launcher's", "[seh_table]")
{
    const auto game = MakeTable(cGameCodeRva, 1000, 0x2000000);
    const auto launcher = MakeTable(cLauncherCodeRva, 300, 0x5500000);

    Plan plan;
    REQUIRE(PlanFor(game, launcher, plan) == Result::kOk);
    CHECK(plan.TableRva == cFreeBegin);
    CHECK(plan.Count == 1300);
    CHECK(plan.SizeBytes == 1300 * sizeof(RuntimeFunction));

    std::vector<RuntimeFunction> merged(plan.Count);
    seh_table::WriteMerged(game.data(), game.size(), launcher.data(), launcher.size(), merged.data());
    CHECK(seh_table::IsSorted(merged.data(), merged.size()));

    const uint32_t cLauncherAddress = launcher[123].BeginAddress + 0x17;
    REQUIRE(Find(merged, cLauncherAddress) != nullptr);
    CHECK(Find(merged, cLauncherAddress)->UnwindData == launcher[123].UnwindData);
    // The game's table alone has no answer for it.
    CHECK(Find(game, cLauncherAddress) == nullptr);

    const uint32_t cGameAddress = game[777].BeginAddress + 0x3F;
    REQUIRE(Find(merged, cGameAddress) != nullptr);
    CHECK(Find(merged, cGameAddress)->UnwindData == game[777].UnwindData);

    CHECK(Find(merged, game[0].EndAddress) == nullptr);
    CHECK(Find(merged, launcher.back().EndAddress) == nullptr);
}

TEST_CASE("the merged exception table refuses tables it cannot keep in order", "[seh_table]")
{
    const auto game = MakeTable(cGameCodeRva, 100, 0x2000000);
    const auto launcher = MakeTable(cLauncherCodeRva, 50, 0x5500000);
    Plan plan;

    CHECK(PlanFor(game, MakeTable(game.back().EndAddress - 1, 50, 0x5500000), plan) == Result::kTablesOverlap);
    CHECK(plan.Count == 0);
    CHECK(PlanFor(launcher, game, plan) == Result::kTablesOverlap);
    CHECK(PlanFor(game, MakeTable(game.back().EndAddress, 50, 0x5500000), plan) == Result::kOk);

    auto unsorted = game;
    std::swap(unsorted[10], unsorted[11]);
    CHECK(PlanFor(unsorted, launcher, plan) == Result::kUnsorted);

    auto emptyRange = launcher;
    emptyRange[5].EndAddress = emptyRange[5].BeginAddress;
    CHECK(PlanFor(game, emptyRange, plan) == Result::kUnsorted);

    CHECK(PlanFor({}, launcher, plan) == Result::kEmpty);
}

TEST_CASE("the merged exception table stays inside its free region", "[seh_table]")
{
    const auto game = MakeTable(cGameCodeRva, 100, 0x2000000);
    const auto launcher = MakeTable(cLauncherCodeRva, 50, 0x5500000);
    const uint32_t cBytes = 150 * sizeof(RuntimeFunction);
    Plan plan;

    CHECK(PlanFor(game, launcher, plan, cFreeBegin, cFreeBegin + cBytes) == Result::kOk);
    CHECK(PlanFor(game, launcher, plan, cFreeBegin, cFreeBegin + cBytes - 1) == Result::kNoRoom);
    CHECK(plan.Count == 0);

    // An unaligned start is rounded up to 16 bytes.
    CHECK(PlanFor(game, launcher, plan, cFreeBegin + 4, cFreeBegin + 16 + cBytes) == Result::kOk);
    CHECK(plan.TableRva == cFreeBegin + 16);
    CHECK(PlanFor(game, launcher, plan, cFreeBegin + 4, cFreeBegin + 15 + cBytes) == Result::kNoRoom);

    CHECK(PlanFor(game, launcher, plan, cFreeEnd, cFreeBegin) == Result::kNoRoom);
    CHECK(PlanFor(game, launcher, plan, 0xFFFFFFF8u, 0xFFFFFFFFu) == Result::kNoRoom);

    CHECK(PlanFor(game, launcher, plan, cGameTableRva - 16, cFreeEnd) == Result::kOverlapsSource);
    CHECK(PlanFor(game, launcher, plan, cLauncherTableRva - 32, cLauncherTableRva + 0x10000) == Result::kOverlapsSource);
}
