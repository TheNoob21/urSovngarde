#include <catch2/catch.hpp>

#include <cmath>
#include <limits>

#include <Services/HealthEase.h>

using namespace HealthEase;

TEST_CASE("a small change in the other player's health is walked to, a big one or none is not", "[healthease]")
{
    CHECK(Eases(80.f, 85.f));
    CHECK(Eases(85.f, 80.f));
    CHECK(Eases(100.f, 100.f + kMaxGap - 0.5f));
    // A heal or a hit of its own: shown at once.
    CHECK_FALSE(Eases(100.f, 100.f + kMaxGap));
    CHECK_FALSE(Eases(100.f, 20.f));
    // Nothing to walk.
    CHECK_FALSE(Eases(80.f, 80.f));
    // Never from or to a broken number.
    const float cNan = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(Eases(cNan, 80.f));
    CHECK_FALSE(Eases(80.f, cNan));
    CHECK_FALSE(Eases(80.f, std::numeric_limits<float>::infinity()));
}

TEST_CASE("the health moves in a straight line over the time the next report takes", "[healthease]")
{
    const Ramp cRamp{80.f, 90.f, 1000.0, 1000.0};
    CHECK(ValueAt(cRamp, 1000.0) == Approx(80.f));
    CHECK(ValueAt(cRamp, 1000.0 + kRampMs / 2.0) == Approx(85.f));
    CHECK(ValueAt(cRamp, 1000.0 + kRampMs) == Approx(90.f));
    // Before the start or long after: the ends, never past them.
    CHECK(ValueAt(cRamp, 500.0) == Approx(80.f));
    CHECK(ValueAt(cRamp, 9000.0) == Approx(90.f));
    CHECK_FALSE(IsDone(cRamp, 1000.0 + kRampMs - 1.0));
    CHECK(IsDone(cRamp, 1000.0 + kRampMs));

    // Downwards the same.
    const Ramp cDown{90.f, 80.f, 0.0, 0.0};
    CHECK(ValueAt(cDown, kRampMs / 4.0) == Approx(87.5f));
}

TEST_CASE("the health is set about thirty times a second while it walks, and at the end", "[healthease]")
{
    Ramp ramp{80.f, 90.f, 0.0, 0.0};
    int sets = 0;
    for (double now = 0.0; now < kRampMs; now += 4.0)
    {
        // Frames come far faster than steps are due.
        if (StepDue(ramp, now))
        {
            ramp.Last = now;
            ++sets;
        }
    }
    CHECK(sets >= static_cast<int>(kRampMs / (kStepMs + 4.0)));
    CHECK(sets <= static_cast<int>(kRampMs / kStepMs) + 1);

    // The last step is never skipped for being too soon after the one before.
    ramp.Last = kRampMs - 1.0;
    CHECK(StepDue(ramp, kRampMs));
}

TEST_CASE("a walk sets only the copy it was made for", "[healthease]")
{
    const Binding cFor{0x10, 3, 0xFF000801};
    const Ramp cRamp{80.f, 90.f, 0.0, 0.0, cFor};
    CHECK(StillFor(cRamp, cFor, true));
    // The actor is no longer another player's copy, or no character has it now.
    CHECK_FALSE(StillFor(cRamp, cFor, false));
    CHECK_FALSE(StillFor(cRamp, Binding{}, true));
    // The form was given to another character, or to the same one under a new epoch, or the character to another form.
    CHECK_FALSE(StillFor(cRamp, Binding{0x11, 3, 0xFF000801}, true));
    CHECK_FALSE(StillFor(cRamp, Binding{0x10, 4, 0xFF000801}, true));
    CHECK_FALSE(StillFor(cRamp, Binding{0x10, 3, 0xFF000802}, true));
    // A walk with no character of its own never writes.
    const Ramp cUnbound{80.f, 90.f, 0.0, 0.0};
    CHECK_FALSE(StillFor(cUnbound, Binding{}, true));
}
