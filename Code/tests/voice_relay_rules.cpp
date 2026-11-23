#include <catch2/catch.hpp>

#include <VoiceRelayRules.h>

using namespace VoiceRelayRules;
using namespace std::chrono_literals;

TEST_CASE("talking at the pace Steam sends is relayed whole", "[voicerelay]")
{
    Budget budget;
    const std::chrono::steady_clock::time_point cStart{100s};
    for (int second = 0; second < 5; ++second)
        for (int piece = 0; piece < 20; ++piece)
            REQUIRE(budget.Allow(cStart + std::chrono::seconds(second) + std::chrono::milliseconds(piece * 50), 400));
}

TEST_CASE("a piece over the size limit or empty is not relayed", "[voicerelay]")
{
    Budget budget;
    const std::chrono::steady_clock::time_point cStart{100s};
    CHECK(budget.Allow(cStart, kMaxPieceBytes));
    CHECK_FALSE(budget.Allow(cStart, kMaxPieceBytes + 1));
    CHECK_FALSE(budget.Allow(cStart, 4096));
    CHECK_FALSE(budget.Allow(cStart, 0));
}

TEST_CASE("the pieces a second are limited", "[voicerelay]")
{
    Budget budget;
    const std::chrono::steady_clock::time_point cStart{100s};
    for (uint32_t i = 0; i < kMaxPiecesPerSecond; ++i)
        REQUIRE(budget.Allow(cStart + std::chrono::milliseconds(i), 10));
    CHECK_FALSE(budget.Allow(cStart + 900ms, 10));
    CHECK(budget.Allow(cStart + 1s, 10));
}

TEST_CASE("the bytes a second are limited, whatever the count", "[voicerelay]")
{
    Budget budget;
    const std::chrono::steady_clock::time_point cStart{100s};
    uint32_t relayed = 0;
    for (int i = 0; i < 100; ++i)
        relayed += budget.Allow(cStart + std::chrono::milliseconds(i), kMaxPieceBytes) ? kMaxPieceBytes : 0;
    CHECK(relayed <= kMaxBytesPerSecond);
    CHECK(relayed > kMaxBytesPerSecond - kMaxPieceBytes);
    CHECK(budget.Allow(cStart + 1s, kMaxPieceBytes));
}

TEST_CASE("a refused piece does not use up the second", "[voicerelay]")
{
    Budget budget;
    const std::chrono::steady_clock::time_point cStart{100s};
    for (int i = 0; i < 100; ++i)
        budget.Allow(cStart, kMaxPieceBytes + 1);
    CHECK(budget.Allow(cStart, 100));
    CHECK(budget.Pieces == 1);
}
