#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>

#include <glm/glm.hpp>

#include <catch2/catch.hpp>

#include <Structs/GameId.h>
#include <Structs/GridCellCoords.h>
#include <ViewerResends.h>

#include <limits>
#include <map>

using namespace ViewerResends;
using namespace std::chrono_literals;

TEST_CASE("A character the player has had since before the shift is not sent again", "[viewer_resends]")
{
    const Clock::time_point cStart{};
    const uint64_t cStamp = Mix(Mix(7, 3), 0);

    // New to the player's range: sent.
    REQUIRE(IsNeeded(nullptr, cStamp, false, true, 1, cStart + 1s));
    const Record cFirst = AfterSend(nullptr, cStamp, false, true, cStart + 1s);
    CHECK(cFirst.NearSends == 1);

    // The next crossing, still in range and unchanged: not sent.
    CHECK_FALSE(IsNeeded(&cFirst, cStamp, true, true, 1, cStart + 12s));
    // Changed since (a new owner, a new epoch): sent.
    CHECK(IsNeeded(&cFirst, Mix(Mix(8, 3), 0), true, true, 1, cStart + 12s));
    // Out of range in between, so removed there: sent.
    CHECK(IsNeeded(&cFirst, cStamp, false, true, 1, cStart + 12s));
    // Long ago: sent, in case the copy was lost.
    CHECK(IsNeeded(&cFirst, cStamp, true, true, 1, cStart + 1s + kRefreshAfter));
}

TEST_CASE("An item or object goes until it has reached the player near, twice", "[viewer_resends]")
{
    const Clock::time_point cStart{};
    const uint64_t cStamp = MixFloat(MixFloat(0, 100.f), 200.f);

    // Far when it came into range: sent once, then not again while far and unchanged.
    Record record = AfterSend(nullptr, cStamp, false, false, cStart);
    CHECK(record.NearSends == 0);
    CHECK_FALSE(IsNeeded(&record, cStamp, true, false, 2, cStart + 10s));

    // The player came near: sent until two sends reached it.
    REQUIRE(IsNeeded(&record, cStamp, true, true, 2, cStart + 10s));
    record = AfterSend(&record, cStamp, true, true, cStart + 10s);
    CHECK(record.NearSends == 1);

    // The cell entry right after the grid shift is the same delivery: it does not count.
    REQUIRE(IsNeeded(&record, cStamp, true, true, 2, cStart + 10s + 200ms));
    record = AfterSend(&record, cStamp, true, true, cStart + 10s + 200ms);
    CHECK(record.NearSends == 1);

    // The next crossing counts.
    REQUIRE(IsNeeded(&record, cStamp, true, true, 2, cStart + 20s));
    record = AfterSend(&record, cStamp, true, true, cStart + 20s);
    CHECK(record.NearSends == 2);
    CHECK_FALSE(IsNeeded(&record, cStamp, true, true, 2, cStart + 30s));

    // Moved: counted afresh.
    const uint64_t cMoved = MixFloat(MixFloat(0, 150.f), 200.f);
    REQUIRE(cMoved != cStamp);
    REQUIRE(IsNeeded(&record, cMoved, true, true, 2, cStart + 31s));
    record = AfterSend(&record, cMoved, true, true, cStart + 31s);
    CHECK(record.NearSends == 1);
}

TEST_CASE("Stamps tell apart what a player's game would take differently", "[viewer_resends]")
{
    CHECK(Mix(Mix(0, 1), 2) != Mix(Mix(0, 2), 1));
    CHECK(Mix(0, 1) != Mix(0, 0));
    CHECK(MixFloat(0, 0.f) != MixFloat(0, -0.f));
    CHECK(MixFloat(5, 1.5f) == MixFloat(5, 1.5f));
}

TEST_CASE("A shift to the same centre is a reannounce and sends everything again", "[viewer_resends]")
{
    const Clock::time_point cStart{};
    const GameId cTamriel{0, 0x3C};
    const GameId cSolstheim{1, 0x800};
    const GridCellCoords cCenter{4, -2};
    const uint64_t cStamp = Mix(Mix(7, 3), 0);

    CHECK(IsReannounce(cTamriel, cCenter, cTamriel, GridCellCoords{4, -2}));
    // A real shift moves the centre; another worldspace or none (indoors, never shifted) is no reannounce.
    CHECK_FALSE(IsReannounce(cTamriel, cCenter, cTamriel, GridCellCoords{5, -2}));
    CHECK_FALSE(IsReannounce(cTamriel, cCenter, cSolstheim, cCenter));
    CHECK_FALSE(IsReannounce(GameId{}, cCenter, GameId{}, cCenter));

    // The records of what the player has, as a server keeps them, after a shift sent it two characters.
    std::map<uint32_t, Record> sent;
    sent[1] = AfterSend(nullptr, cStamp, false, true, cStart);
    sent[2] = AfterSend(nullptr, cStamp, false, true, cStart);

    const auto shift = [&](const GridCellCoords& acPrevious, const GridCellCoords& acCenter, const Clock::time_point aNow)
    {
        if (IsReannounce(cTamriel, acPrevious, cTamriel, acCenter))
            sent.clear();
        int sends = 0;
        for (const uint32_t cId : {1u, 2u})
        {
            const auto cRecord = sent.find(cId);
            const Record* pRecord = cRecord != sent.end() ? &cRecord->second : nullptr;
            if (!IsNeeded(pRecord, cStamp, true, true, 1, aNow))
                continue;
            sent[cId] = AfterSend(pRecord, cStamp, true, true, aNow);
            ++sends;
        }
        return sends;
    };

    // A real shift a few seconds later still skips what the player has.
    CHECK(shift(cCenter, GridCellCoords{5, -2}, cStart + 6s) == 0);
    // Its game lost a copy and announces the same centre again: everything goes.
    CHECK(shift(GridCellCoords{5, -2}, GridCellCoords{5, -2}, cStart + 8s) == 2);
    // The next real shift skips again.
    CHECK(shift(GridCellCoords{5, -2}, GridCellCoords{6, -2}, cStart + 12s) == 0);
}

TEST_CASE("A character's stamp holds roughly where it is, and it is refreshed sooner", "[viewer_resends]")
{
    const Clock::time_point cStart{};
    const uint64_t cBase = Mix(Mix(7, 3), 0);

    // Within one cell of the place grid: the same stamp. Into the next, or far off: another.
    CHECK(MixPlace(cBase, 10.f, 20.f, 0.f) == MixPlace(cBase, 500.f, 300.f, 100.f));
    CHECK(MixPlace(cBase, 10.f, 20.f, 0.f) != MixPlace(cBase, 10.f + kPlaceCell, 20.f, 0.f));
    CHECK(MixPlace(cBase, 10.f, 20.f, 0.f) != MixPlace(cBase, -10.f, 20.f, 0.f));
    CHECK(MixPlace(cBase, 10.f, 20.f, 0.f) != MixPlace(cBase, 10.f, 20.f, -kPlaceCell));
    CHECK(MixPlace(cBase, 10.f, 20.f, 0.f) != MixPlace(cBase, 20.f, 10.f, 0.f + 3 * kPlaceCell));
    // Not a number: a stamp all the same.
    CHECK(MixPlace(cBase, std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f) == MixPlace(cBase, 0.f, 0.f, 0.f));

    // A copy whose character walked a few steps at the next crossing is not sent again; one that went far is.
    const uint64_t cThen = MixPlace(cBase, 100.f, 100.f, 0.f);
    const Record cSent = AfterSend(nullptr, cThen, false, true, cStart);
    CHECK_FALSE(IsNeeded(&cSent, MixPlace(cBase, 180.f, 140.f, 0.f), true, true, 1, cStart + 6s, kCharacterRefreshAfter));
    CHECK(IsNeeded(&cSent, MixPlace(cBase, 2100.f, 100.f, 0.f), true, true, 1, cStart + 6s, kCharacterRefreshAfter));

    // Unchanged, a character goes again after kCharacterRefreshAfter; items and objects after kRefreshAfter.
    CHECK_FALSE(IsNeeded(&cSent, cThen, true, true, 1, cStart + kCharacterRefreshAfter - 1s, kCharacterRefreshAfter));
    CHECK(IsNeeded(&cSent, cThen, true, true, 1, cStart + kCharacterRefreshAfter, kCharacterRefreshAfter));
    CHECK_FALSE(IsNeeded(&cSent, cThen, true, true, 1, cStart + kCharacterRefreshAfter));
}
