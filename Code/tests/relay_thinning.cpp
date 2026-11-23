#include <catch2/catch.hpp>

#include <RelayThinning.h>

using namespace RelayThinning;

TEST_CASE("Players farther from an actor get fewer of its updates, near ones all", "[relay_thinning]")
{
    CHECK(BandOf(0.f) == Band::kNear);
    CHECK(BandOf(kMidDistance) == Band::kNear);
    CHECK(BandOf(kMidDistance + 1.f) == Band::kMid);
    CHECK(BandOf(kFarDistance) == Band::kMid);
    CHECK(BandOf(kFarDistance + 1.f) == Band::kFar);
    // Elsewhere, or no place known: nothing is held back.
    CHECK(BandOf(std::nullopt) == Band::kNear);

    for (const bool cMidDue : {false, true})
        for (const bool cFarDue : {false, true})
            CHECK(Relays(Band::kNear, cMidDue, cFarDue, false));
    CHECK_FALSE(Relays(Band::kMid, false, true, false));
    CHECK(Relays(Band::kMid, true, false, false));
    CHECK_FALSE(Relays(Band::kFar, true, false, false));
    CHECK(Relays(Band::kFar, false, true, false));
    // Fingers, scale, weapons and the hand check come once a second at most: always passed on.
    CHECK(Relays(Band::kFar, false, false, true));
    CHECK(Relays(Band::kMid, false, false, true));
}

TEST_CASE("At 30 updates a second, half go a little way off and a quarter far off", "[relay_thinning]")
{
    uint64_t lastMid = 0;
    uint64_t lastFar = 0;
    int midSent = 0;
    int farSent = 0;
    // 33 or 34 ms apart, as a game's frames come.
    uint64_t tick = 1000;
    for (int i = 0; i < 120; ++i)
    {
        tick += (i % 3 == 0) ? 34 : 33;
        if (IsDue(tick, lastMid, kMidIntervalMs))
        {
            lastMid = tick;
            ++midSent;
        }
        if (IsDue(tick, lastFar, kFarIntervalMs))
        {
            lastFar = tick;
            ++farSent;
        }
    }
    CHECK(midSent == 60);
    CHECK(farSent == 30);

    // Updates already slower than the interval all go.
    CHECK(IsDue(1200, 1000, kFarIntervalMs));
    // The first, and one on another owner's clock after a hand-over.
    CHECK(IsDue(50, 0, kFarIntervalMs));
    CHECK(IsDue(500, 90000, kFarIntervalMs));
    CHECK_FALSE(IsDue(1050, 1000, kMidIntervalMs));
}
