#include <catch2/catch.hpp>

#include <Games/Skyrim/TakeoverRules.h>

#include <limits>
#include <vector>

using namespace TakeoverRules;

namespace
{
// Readings every 125 ms up to aNowMs: the body goes aSpeed units a second along X, and each foot swings by aStride.
std::vector<Sample> Readings(int64_t aNowMs, float aSpeed, float aStride, int64_t aEveryMs = kSampleEveryMs)
{
    std::vector<Sample> samples;
    for (int64_t t = aNowMs - kWindowMs; t <= aNowMs; t += aEveryMs)
    {
        const float cSwing = (t / aEveryMs) % 2 ? aStride : 0.f;
        samples.push_back({t, aSpeed * static_cast<float>(t) / 1000.f, 0.f, 20.f + cSwing, 20.f + aStride - cSwing});
    }
    return samples;
}
} // namespace

TEST_CASE("A gained NPC's legs are judged against how far its body goes", "[takeover]")
{
    SECTION("travel on still legs is a slide")
    {
        CHECK(Judge(Readings(2000, 70.f, 0.f), 2000) == Legs::kSlide);
        // Feet that settle by a unit or two are still standing legs.
        CHECK(Judge(Readings(2000, 70.f, 2.f), 2000) == Legs::kSlide);
    }
    SECTION("travel with a stepping foot is a walk")
    {
        CHECK(Judge(Readings(2000, 70.f, 15.f), 2000) == Legs::kWalk);
        CHECK(Judge(Readings(2000, 70.f, kMinStride), 2000) == Legs::kWalk);
    }
    SECTION("a body that does not travel stands, whatever its legs do")
    {
        CHECK(Judge(Readings(2000, 0.f, 0.f), 2000) == Legs::kStand);
        CHECK(Judge(Readings(2000, 10.f, 15.f), 2000) == Legs::kStand);
    }
    SECTION("too few readings, or readings that do not cover the window, say nothing")
    {
        CHECK(Judge({}, 2000) == Legs::kUnknown);
        CHECK(Judge(Readings(2000, 70.f, 0.f, 375), 2000) == Legs::kUnknown);
        auto samples = Readings(2000, 70.f, 0.f);
        samples.erase(samples.begin(), samples.begin() + 4);
        CHECK(Judge(samples, 2000) == Legs::kUnknown);
    }
    SECTION("readings older than the window, from the future, or not finite are left out")
    {
        auto samples = Readings(2000, 0.f, 0.f);
        samples.insert(samples.begin(), Sample{100, -500.f, 0.f, 20.f, 20.f});
        samples.push_back({2600, 900.f, 0.f, 20.f, 20.f});
        samples.push_back({2000, std::numeric_limits<float>::quiet_NaN(), 0.f, 20.f, 20.f});
        CHECK(Judge(samples, 2000) == Legs::kStand);
    }
    SECTION("a slow walk is not missed and a standing sway is not taken for travel")
    {
        // 27 units a second is the slowest travel judged; a sway of a few units back and forth is not travel.
        CHECK(Judge(Readings(2000, 30.f, 0.f), 2000) == Legs::kSlide);
        std::vector<Sample> sway;
        for (int64_t t = 1250; t <= 2000; t += kSampleEveryMs)
            sway.push_back({t, (t / kSampleEveryMs) % 2 ? 1.f : 0.f, 0.f, 20.f, 20.f});
        CHECK(Judge(sway, 2000) == Legs::kStand);
    }
}

TEST_CASE("A sliding gained NPC is started, then reset and started, then left alone", "[takeover]")
{
    CHECK(NextMend(Legs::kSlide, true, 0, true) == Mend::kStart);
    CHECK(NextMend(Legs::kSlide, true, 1, true) == Mend::kResetStart);
    CHECK(NextMend(Legs::kSlide, true, 2, true) == Mend::kGiveUp);
    CHECK(NextMend(Legs::kSlide, true, 3, true) == Mend::kNone);
    // A body this game's AI gives no speed is not its walk: pushed or carried, it is left standing.
    for (uint32_t mends = 0; mends <= kMaxMends; ++mends)
        CHECK(NextMend(Legs::kSlide, false, mends, true) == Mend::kNone);
    // Each mend waits a window of new readings.
    CHECK(NextMend(Legs::kSlide, true, 1, false) == Mend::kNone);
    // Walking, standing or not yet judged: nothing to mend.
    for (const Legs cLegs : {Legs::kWalk, Legs::kStand, Legs::kUnknown})
        for (uint32_t mends = 0; mends <= kMaxMends; ++mends)
            CHECK(NextMend(cLegs, true, mends, true) == Mend::kNone);
}

TEST_CASE("Only an NPC this game runs, on its feet, alive and up is mended", "[takeover]")
{
    CHECK(CanMend(true, true, false, false, false, false));
    CHECK_FALSE(CanMend(false, true, false, false, false, false));
    CHECK_FALSE(CanMend(true, false, false, false, false, false));
    CHECK_FALSE(CanMend(true, true, true, false, false, false));
    CHECK_FALSE(CanMend(true, true, false, true, false, false));
    CHECK_FALSE(CanMend(true, true, false, false, true, false));
    CHECK_FALSE(CanMend(true, true, false, false, false, true));
}

TEST_CASE("The watch is short and quick enough to start legs well inside a second", "[takeover]")
{
    // The first judgement can come one window after the first look.
    CHECK(kFirstLookMs + kWindowMs <= 1100);
    CHECK(kWindowMs / kSampleEveryMs >= 4);
    CHECK(kWatchMs >= kFirstLookMs + kWindowMs * (kMaxMends + 1));
}

TEST_CASE("A new copy is started only while its owner's reports walk it, on its owner's own graph", "[takeover]")
{
    CHECK(OwnerMoves(60.f));
    CHECK_FALSE(OwnerMoves(kOwnerMovingSpeed));
    CHECK_FALSE(OwnerMoves(0.f));
    CHECK_FALSE(OwnerMoves(std::numeric_limits<float>::quiet_NaN()));

    CHECK(CanMendCopy(true, false, true, false, false, false, false));
    // A player's copy, another kind's graph, or a body moved on purpose.
    CHECK_FALSE(CanMendCopy(false, false, true, false, false, false, false));
    CHECK_FALSE(CanMendCopy(true, true, true, false, false, false, false));
    CHECK_FALSE(CanMendCopy(true, false, false, false, false, false, false));
    CHECK_FALSE(CanMendCopy(true, false, true, true, false, false, false));
    CHECK_FALSE(CanMendCopy(true, false, true, false, true, false, false));
    CHECK_FALSE(CanMendCopy(true, false, true, false, false, true, false));
    CHECK_FALSE(CanMendCopy(true, false, true, false, false, false, true));

    // The same slide judgement and mend order as a gained NPC, with the owner's reports as the walk.
    CHECK(NextMend(Judge(Readings(2000, 70.f, 0.f), 2000), OwnerMoves(70.f), 0, true) == Mend::kStart);
    CHECK(NextMend(Judge(Readings(2000, 70.f, 0.f), 2000), OwnerMoves(0.f), 0, true) == Mend::kNone);
}
