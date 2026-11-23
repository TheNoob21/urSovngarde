#include <catch2/catch.hpp>

#include <Services/ChestSelection.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace
{
bool Has(const std::vector<ChestSelection::Cell>& acCells, int32_t aX, int32_t aY)
{
    return std::any_of(acCells.begin(), acCells.end(), [aX, aY](const ChestSelection::Cell& acCell) { return acCell.X == aX && acCell.Y == aY; });
}
} // namespace

TEST_CASE("a game announces an object once while it stays in the cells around its player", "[chest-ledger]")
{
    ChestSelection::Announced announced;

    REQUIRE(announced.Update({0x34EE1, 0x34EE2, 0x34EE3}) == std::vector<size_t>{0, 1, 2});
    REQUIRE(announced.Update({0x34EE1, 0x34EE2, 0x34EE3}).empty());
    REQUIRE(announced.Size() == 3);

    SECTION("crossing a cell announces only the new strip")
    {
        // 0x34EE1 is left behind, 0x34EE4 comes into the cells.
        REQUIRE(announced.Update({0x34EE2, 0x34EE3, 0x34EE4}) == std::vector<size_t>{2});
        REQUIRE(announced.Size() == 3);
    }

    SECTION("one that left the cells is announced again when it comes back, as the server forgot it")
    {
        REQUIRE(announced.Update({0x34EE2}).empty());
        REQUIRE(announced.Update({0x34EE1, 0x34EE2}) == std::vector<size_t>{0});
    }

    SECTION("after a new connection every object is announced again")
    {
        announced.Clear();
        REQUIRE(announced.Size() == 0);
        REQUIRE(announced.Update({0x34EE3, 0x34EE1}) == std::vector<size_t>{0, 1});
    }

    SECTION("an object found twice is announced once")
    {
        REQUIRE(announced.Update({0x34EE5, 0x34EE5, 0x34EE1}) == std::vector<size_t>{0});
    }

    SECTION("with no cells, nothing is announced and everything is forgotten")
    {
        REQUIRE(announced.Update({}).empty());
        REQUIRE(announced.Size() == 0);
    }
}

TEST_CASE("an object's entity stays only while it is in the announced cells", "[chest-ledger]")
{
    ChestSelection::Announced announced;
    announced.Update({0x34EE1, 0x34EE2});
    REQUIRE(announced.Contains(0x34EE1));
    REQUIRE(announced.Contains(0x34EE2));
    REQUIRE_FALSE(announced.Contains(0x34EE3));

    SECTION("one left behind as the player crosses a cell goes")
    {
        announced.Update({0x34EE2, 0x34EE3});
        REQUIRE_FALSE(announced.Contains(0x34EE1));
        REQUIRE(announced.Contains(0x34EE2));
        REQUIRE(announced.Contains(0x34EE3));
    }

    SECTION("all go on a new connection or in a player home")
    {
        announced.Clear();
        REQUIRE_FALSE(announced.Contains(0x34EE1));
        REQUIRE_FALSE(announced.Contains(0x34EE2));
    }
}

TEST_CASE("outdoors the objects of the 3x3 cells around the player are announced", "[chest-ledger]")
{
    REQUIRE(ChestSelection::cCellsOut == 1);

    const std::vector<ChestSelection::Cell> cCells = ChestSelection::CellsAround(5, -11, ChestSelection::cCellsOut);
    REQUIRE(cCells.size() == 9);
    // The player's own cell, and the next one over where a neighbour chest stands.
    REQUIRE(Has(cCells, 5, -11));
    REQUIRE(Has(cCells, 5, -12));
    REQUIRE(Has(cCells, 4, -12));
    REQUIRE(Has(cCells, 6, -10));
    REQUIRE_FALSE(Has(cCells, 7, -11));
    REQUIRE_FALSE(Has(cCells, 5, -13));

    REQUIRE(ChestSelection::CellsAround(5, -11, 0).size() == 1);
}

TEST_CASE("a large announcement goes in requests of at most 64 objects", "[chest-ledger]")
{
    // A request counts its objects in 8 bits.
    REQUIRE(ChestSelection::cMaxPerRequest == 64);
    static_assert(ChestSelection::cMaxPerRequest <= 255, "a request counts its objects in 8 bits");

    REQUIRE(ChestSelection::Batches(0, 64).empty());
    REQUIRE(ChestSelection::Batches(1, 64) == std::vector<std::pair<size_t, size_t>>{{0, 1}});
    REQUIRE(ChestSelection::Batches(64, 64) == std::vector<std::pair<size_t, size_t>>{{0, 64}});
    REQUIRE(ChestSelection::Batches(65, 64) == std::vector<std::pair<size_t, size_t>>{{0, 64}, {64, 65}});

    const auto cBatches = ChestSelection::Batches(1000, ChestSelection::cMaxPerRequest);
    REQUIRE(cBatches.size() == 16);
    REQUIRE(cBatches.front().first == 0);
    REQUIRE(cBatches.back().second == 1000);
    for (size_t i = 0; i < cBatches.size(); ++i)
    {
        REQUIRE(cBatches[i].second - cBatches[i].first <= ChestSelection::cMaxPerRequest);
        if (i > 0)
            REQUIRE(cBatches[i].first == cBatches[i - 1].second);
    }
}
