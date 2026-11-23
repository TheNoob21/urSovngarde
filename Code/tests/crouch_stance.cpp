#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>

#include <catch2/catch.hpp>

#include <Systems/CrouchStance.h>

#include <limits>

using namespace CrouchStance;

namespace
{
// Feeds aHip every 11 ms (90 fps) from aFrom for aFor ms; returns the stance after the last reading.
bool Feed(Watch& aWatch, const float aHip, const double aFrom, const double aFor, const bool aAllowed = true)
{
    bool crouched = aWatch.Crouched;
    for (double t = aFrom; t <= aFrom + aFor; t += 11.0)
        crouched = Update(aWatch, aAllowed, aHip, t);
    return crouched;
}
} // namespace

TEST_CASE("A VR player standing still is never shown crouched", "[crouch_stance]")
{
    Watch watch;
    REQUIRE_FALSE(Feed(watch, 68.f, 0.0, 5000.0));
    REQUIRE(watch.Standing == Approx(68.f));
    // Swaying a little is still standing.
    REQUIRE_FALSE(Feed(watch, 62.f, 5000.0, 2000.0));
}

TEST_CASE("A real crouch shows after a short hold and stands again past the higher line", "[crouch_stance]")
{
    Watch watch;
    Feed(watch, 68.f, 0.0, 1000.0);

    // Down well past three quarters of the standing height: not at once, then after the hold.
    REQUIRE_FALSE(Update(watch, true, 40.f, 1100.0));
    REQUIRE_FALSE(Update(watch, true, 40.f, 1100.0 + kHoldMs / 2.0));
    REQUIRE(Update(watch, true, 40.f, 1100.0 + kHoldMs));

    // Between the two lines it stays crouched (68 * 0.75 = 51, 68 * 0.85 = 57.8).
    REQUIRE(Feed(watch, 55.f, 1500.0, 2000.0));
    // A long crouch does not wear the standing height down.
    REQUIRE(Feed(watch, 40.f, 3600.0, 30000.0));
    REQUIRE(watch.Standing == Approx(68.f));

    // Up past the higher line: standing again after the hold.
    REQUIRE(Update(watch, true, 66.f, 34000.0));
    REQUIRE_FALSE(Update(watch, true, 66.f, 34000.0 + kHoldMs));
}

TEST_CASE("A quick bob or one bad pose does not flicker the stance", "[crouch_stance]")
{
    Watch watch;
    Feed(watch, 68.f, 0.0, 1000.0);
    // Down for less than the hold, then back up: never crouched.
    REQUIRE_FALSE(Feed(watch, 40.f, 1011.0, kHoldMs - 50.0));
    REQUIRE_FALSE(Feed(watch, 68.f, 1300.0, 500.0));

    // Crouched, one missing hip reading does not stand it.
    Feed(watch, 40.f, 2000.0, 1000.0);
    REQUIRE(watch.Crouched);
    REQUIRE(Update(watch, true, std::numeric_limits<float>::quiet_NaN(), 3011.0));
    REQUIRE(Update(watch, true, 40.f, 3022.0));
    // Readings lost for good: it stands after the hold.
    REQUIRE_FALSE(Feed(watch, std::numeric_limits<float>::quiet_NaN(), 3033.0, kHoldMs + 50.0));
}

TEST_CASE("One high spike does not make standing read as a crouch", "[crouch_stance]")
{
    Watch watch;
    Feed(watch, 68.f, 0.0, 1000.0);
    // A single 200 for one frame raises the standing height only a little.
    Update(watch, true, 200.f, 1011.0);
    REQUIRE(watch.Standing < 72.f);
    REQUIRE_FALSE(Feed(watch, 68.f, 1022.0, 2000.0));
}

TEST_CASE("The standing height follows a body that starts crouched or turns out smaller", "[crouch_stance]")
{
    // First seen crouched: that is its standing height until it stands up, so it is not shown crouched.
    Watch watch;
    REQUIRE_FALSE(Feed(watch, 40.f, 0.0, 1000.0));
    // It stands up: the standing height rises to it within a second.
    REQUIRE_FALSE(Feed(watch, 68.f, 1011.0, 1000.0));
    REQUIRE(watch.Standing == Approx(68.f));
    // And now a crouch shows.
    REQUIRE(Feed(watch, 40.f, 2100.0, 500.0));

    // A standing height settles slowly to a slightly lower standing reading.
    Watch smaller;
    Feed(smaller, 70.f, 0.0, 500.0);
    Feed(smaller, 64.f, 511.0, 10000.0);
    REQUIRE(smaller.Standing == Approx(64.f).margin(0.5f));
}

TEST_CASE("Leg trackers, the sneak toggle or a copy off its feet stand it at once", "[crouch_stance]")
{
    Watch watch;
    Feed(watch, 68.f, 0.0, 1000.0);
    Feed(watch, 40.f, 1011.0, 500.0);
    REQUIRE(watch.Crouched);
    REQUIRE_FALSE(Update(watch, false, 40.f, 1600.0));
    REQUIRE(watch.Standing == Approx(68.f));
    // While not allowed nothing is shown, however low.
    REQUIRE_FALSE(Feed(watch, 30.f, 1611.0, 1000.0, false));
    // Allowed again while still low: after the hold.
    REQUIRE_FALSE(Update(watch, true, 40.f, 2700.0));
    REQUIRE(Update(watch, true, 40.f, 2700.0 + kHoldMs));
}

TEST_CASE("A body too short to be one is never shown crouched", "[crouch_stance]")
{
    Watch watch;
    REQUIRE_FALSE(Feed(watch, 20.f, 0.0, 1000.0));
    REQUIRE_FALSE(Feed(watch, 5.f, 1011.0, 1000.0));
}

TEST_CASE("The sneak stance's variables are written over the owner's, and the owner's own sneak is read", "[crouch_stance]")
{
    AnimationVariables variables;
    variables.Booleans = {false, false, false};
    variables.Integers = {0, 0};
    REQUIRE_FALSE(OwnerSneaks(variables, 1));
    Hold(variables, 1, 0);
    REQUIRE(static_cast<bool>(variables.Booleans[1]));
    REQUIRE_FALSE(static_cast<bool>(variables.Booleans[0]));
    REQUIRE(variables.Integers[0] == 1u);
    REQUIRE(OwnerSneaks(variables, 1));

    // Slots the graph has not, or past the lists: left alone.
    AnimationVariables none;
    none.Booleans = {false};
    Hold(none, -1, 5);
    Hold(none, 3, -1);
    REQUIRE_FALSE(static_cast<bool>(none.Booleans[0]));
    REQUIRE_FALSE(OwnerSneaks(none, -1));
    REQUIRE_FALSE(OwnerSneaks(none, 4));
}

TEST_CASE("Seated, sleeping, riding or swimming is no crouch", "[crouch_stance]")
{
    // Standing on its feet, and sneaking (bit 9) or holding a weapon, is not low for another reason.
    CHECK_FALSE(LowForOtherReason(0, false));
    CHECK_FALSE(LowForOtherReason(1u << 9, false));
    // Each sit/sleep state, settled or on the way in or out (bits 14-17).
    for (uint32_t state = 1; state <= 8; ++state)
        CHECK(LowForOtherReason(state << 14, false));
    CHECK(LowForOtherReason(kSwimmingBit, false));
    CHECK(LowForOtherReason(0, true));

    // Low hips while seated never show the stance, and the standing height is kept for after.
    Watch watch;
    Feed(watch, 68.f, 0.0, 1000.0);
    REQUIRE_FALSE(Feed(watch, 35.f, 1000.0, 2000.0, !LowForOtherReason(3u << 14, false)));
    CHECK(watch.Standing == Approx(68.f));
}
