#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>

#include <catch2/catch.hpp>

#include <Systems/StrideMatch.h>

#include <cmath>
#include <limits>

using namespace StrideMatch;

TEST_CASE("A copy's ground speed measures the displayed frame including correction", "[stride_match]")
{
    // A jog: 350 units a second, sent 30 times a second.
    const auto cJog = GroundSpeed({0.f, 0.f, 0.f}, {7.f, 0.f, 0.f}, 20.0);
    REQUIRE(cJog);
    REQUIRE(*cJog == Approx(350.f));

    // Height is left out: a jog down a slope is still a jog.
    const auto cSlope = GroundSpeed({0.f, 0.f, 0.f}, {3.f, 4.f, -20.f}, 100.0);
    REQUIRE(cSlope);
    REQUIRE(*cSlope == Approx(50.f));

    // No time between them, or the points the wrong way round, or a snap: none.
    REQUIRE_FALSE(GroundSpeed({}, {10.f, 0.f, 0.f}, 0.0));
    REQUIRE_FALSE(GroundSpeed({}, {10.f, 0.f, 0.f}, 50.0, true));
    REQUIRE_FALSE(GroundSpeed({}, {10.f, 0.f, 0.f}, 300.0));
    REQUIRE_FALSE(GroundSpeed({}, {1000.f, 0.f, 0.f}, 100.0));
    REQUIRE_FALSE(GroundSpeed({}, {std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f}, 100.0));

    // Standing still is a speed of 0, not none.
    const auto cStill = GroundSpeed({5.f, 5.f, 0.f}, {5.f, 5.f, 0.f}, 33.0);
    REQUIRE(cStill);
    REQUIRE(*cStill == 0.f);
}

TEST_CASE("The shown speed eases toward each new sample without pumping", "[stride_match]")
{
    // The first sample is taken whole.
    REQUIRE(Ease(-1.f, 350.f, 11.0) == 350.f);
    REQUIRE(Ease(std::numeric_limits<float>::quiet_NaN(), 350.f, 11.0) == 350.f);

    // A bad sample or no time leaves it as it was.
    REQUIRE(Ease(100.f, std::numeric_limits<float>::quiet_NaN(), 11.0) == 100.f);
    REQUIRE(Ease(100.f, 350.f, 0.0) == 100.f);

    // After one ease time it has covered about 63% of a step, and it never overshoots.
    REQUIRE(Ease(0.f, 100.f, kEaseMs) == Approx(63.2f).margin(0.1f));
    float shown = 82.f;
    for (int frame = 0; frame < 90; ++frame)
    {
        shown = Ease(shown, 350.f, 1000.0 / 90.0);
        REQUIRE(shown <= 350.f);
    }
    REQUIRE(shown == Approx(350.f).margin(1.f));

    // Whole-unit positions at 30 a second give samples 30 units a second either side of a steady jog. Eased at 90 frames
    // a second, the legs' speed stays well inside that band.
    shown = 350.f;
    float lowest = shown;
    float highest = shown;
    for (int frame = 0; frame < 900; ++frame)
    {
        const float cSample = (frame / 3) % 2 == 0 ? 380.f : 320.f;
        shown = Ease(shown, cSample, 1000.0 / 90.0);
        lowest = std::min(lowest, shown);
        highest = std::max(highest, shown);
    }
    REQUIRE(highest - lowest < 15.f);
}

TEST_CASE("The legs follow the shown speed and stop when the copy stops", "[stride_match]")
{
    // Shown jogging while its owner's graph said walking (O9): it jogs.
    REQUIRE(LegSpeed(350.f, 82.f) == 350.f);
    // Shown walking while its owner's graph said sprinting: it walks.
    REQUIRE(LegSpeed(80.f, 520.f) == 80.f);
    REQUIRE(LegSpeed(0.f, 82.f) == 0.f);
    REQUIRE(LegSpeed(kStillSpeed - 1.f, 82.f) == 0.f);
    // Nothing known yet, or nothing sensible from the owner.
    REQUIRE(LegSpeed(-1.f, 120.f) == 120.f);
    REQUIRE(LegSpeed(-1.f, -5.f) == 0.f);
    REQUIRE(LegSpeed(-1.f, std::numeric_limits<float>::infinity()) == 0.f);
    REQUIRE(LegSpeed(std::numeric_limits<float>::quiet_NaN(), 50.f) == 50.f);
}

TEST_CASE("The legs' speed goes into the Speed and sampled-speed slots only", "[stride_match]")
{
    AnimationVariables variables;
    // The humanoid order: TurnDelta, Direction, SpeedSampled, weapAdj, Speed.
    variables.Floats = {0.1f, 0.25f, 82.f, 1.f, 82.f};
    Drive(variables, 4, 2, 350.f);
    REQUIRE(variables.Floats[0] == 0.1f);
    REQUIRE(variables.Floats[1] == 0.25f);
    REQUIRE(variables.Floats[2] == 350.f);
    REQUIRE(variables.Floats[3] == 1.f);
    REQUIRE(variables.Floats[4] == 350.f);

    // A graph without a sampled speed, or a slot past the list (another graph's list): only what is there is written.
    Drive(variables, 4, -1, 120.f);
    REQUIRE(variables.Floats[2] == 350.f);
    REQUIRE(variables.Floats[4] == 120.f);
    Drive(variables, 9, 7, 500.f);
    REQUIRE(variables.Floats.size() == 5);
    REQUIRE(variables.Floats[4] == 120.f);
}

TEST_CASE("Legs and owner's graph disagree only by a clear margin", "[stride_match]")
{
    REQUIRE(Mismatch(350.f, 82.f));
    REQUIRE(Mismatch(82.f, 350.f));
    REQUIRE_FALSE(Mismatch(350.f, 330.f));
    REQUIRE_FALSE(Mismatch(30.f, 0.f));
    REQUIRE_FALSE(Mismatch(0.f, 0.f));
}

TEST_CASE("A copy's sprint ends once it has been shown slower than a sprint for a while", "[stride_match]")
{
    SprintWatch watch;
    // Sprinting at sprint speed: never.
    for (double now = 0.0; now < 2000.0; now += 11.0)
        REQUIRE_FALSE(StopsSprint(watch, true, 400.f, now));

    // Slowed to a jog with the sprint still on: asked once, after kSprintSlowMs.
    int stops = 0;
    double stoppedAt = -1.0;
    for (double now = 2000.0; now < 4000.0; now += 10.0)
        if (StopsSprint(watch, true, 150.f, now))
        {
            ++stops;
            if (stoppedAt < 0.0)
                stoppedAt = now;
        }
    REQUIRE(stoppedAt >= 2000.0 + kSprintSlowMs);
    REQUIRE(stoppedAt < 2000.0 + kSprintSlowMs + 20.0);
    // Still shown sprinting by its graph two seconds on (the stop was refused): asked again, at most every kSprintSlowMs.
    REQUIRE(stops <= static_cast<int>(2000.0 / kSprintSlowMs));

    // A short dip, as at the start of a sprint, is left alone.
    watch = {};
    REQUIRE_FALSE(StopsSprint(watch, true, 150.f, 0.0));
    REQUIRE_FALSE(StopsSprint(watch, true, 150.f, kSprintSlowMs - 50.0));
    REQUIRE_FALSE(StopsSprint(watch, true, 450.f, kSprintSlowMs + 10.0));
    REQUIRE_FALSE(StopsSprint(watch, true, 150.f, kSprintSlowMs + 20.0));
    REQUIRE_FALSE(StopsSprint(watch, true, 150.f, 2.0 * kSprintSlowMs));

    // Shown at 220 to 370 units a second, below its own sprint clip's pace: ended too, as the run's clips have that pace and
    // the sprint clip's feet would run ahead of the body.
    watch = {};
    REQUIRE_FALSE(StopsSprint(watch, true, 300.f, 0.0));
    REQUIRE(StopsSprint(watch, true, 300.f, kSprintSlowMs));
    // At the game's own sprint (500) or a sprint slowed but still above a run: left alone.
    watch = {};
    for (double now = 0.0; now < 2000.0; now += 11.0)
        REQUIRE_FALSE(StopsSprint(watch, true, kSprintFloor + 1.f, now));

    // Not sprinting: never, however slow.
    watch = {};
    for (double now = 0.0; now < 2000.0; now += 11.0)
        REQUIRE_FALSE(StopsSprint(watch, false, 0.f, now));

    // A speed that is not a number is not slow.
    watch = {};
    REQUIRE_FALSE(StopsSprint(watch, true, std::numeric_limits<float>::quiet_NaN(), 0.0));
    REQUIRE_FALSE(StopsSprint(watch, true, std::numeric_limits<float>::quiet_NaN(), 1000.0));
}

TEST_CASE("A lasting disagreement between legs and owner's graph is said once per spell", "[stride_match]")
{
    MismatchWatch watch;
    // A start or stop: the eased legs lag for a moment, which is not said.
    REQUIRE_FALSE(SayMismatch(watch, true, 0.0));
    REQUIRE_FALSE(SayMismatch(watch, true, kMismatchSayMs - 1.0));
    REQUIRE_FALSE(SayMismatch(watch, false, kMismatchSayMs));

    // One that lasts is said once.
    int said = 0;
    for (double now = 1000.0; now < 5000.0; now += 11.0)
        said += SayMismatch(watch, true, now) ? 1 : 0;
    REQUIRE(said == 1);

    // Over, then back: said again.
    REQUIRE_FALSE(SayMismatch(watch, false, 5000.0));
    REQUIRE_FALSE(SayMismatch(watch, true, 5100.0));
    REQUIRE(SayMismatch(watch, true, 5100.0 + kMismatchSayMs));
}

TEST_CASE("Legs are matched for humanoid copies on foot that another game moves, not mounts or other graphs", "[stride_match]")
{
    // A remote player or an NPC copy, walking.
    REQUIRE(Applies(true, false, true, true));
    // Run by this game, a horse, sitting or riding, or a creature's or beast form's own graph.
    REQUIRE_FALSE(Applies(false, false, true, true));
    REQUIRE_FALSE(Applies(true, true, true, true));
    REQUIRE_FALSE(Applies(true, false, false, true));
    REQUIRE_FALSE(Applies(true, false, true, false));
}

TEST_CASE("A replayed action keeps the legs' speed instead of its owner's", "[stride_match]")
{
    // The owner's action carries the speed its graph had then; the copy's legs keep the speed they are given.
    AnimationVariables action;
    action.Floats = {0.f, 0.f, 82.f, 1.f, 82.f};
    Drive(action, 4, 2, 350.f);
    REQUIRE(action.Floats[2] == 350.f);
    REQUIRE(action.Floats[4] == 350.f);
    REQUIRE(action.Floats[3] == 1.f);
}

TEST_CASE("A roomscale step is read against the body's facing, so a sidestep and a step back are not a walk forward", "[stride_match]")
{
    constexpr float cQuarter = kFullTurn / 4.f;
    // Facing +y (heading 0): forward, right (+x), back, left.
    REQUIRE(*StepDirection({}, {0.f, 5.f, 0.f}, 0.f) == Approx(0.f).margin(1e-5));
    REQUIRE(*StepDirection({}, {5.f, 0.f, 0.f}, 0.f) == Approx(0.25f));
    REQUIRE(*StepDirection({}, {0.f, -5.f, 0.f}, 0.f) == Approx(0.5f));
    REQUIRE(*StepDirection({}, {-5.f, 0.f, 0.f}, 0.f) == Approx(0.75f));

    // Facing +x (a quarter turn clockwise): a step along +x is forward, one along -y is to the right.
    REQUIRE(*StepDirection({10.f, 10.f, 0.f}, {15.f, 10.f, 0.f}, cQuarter) == Approx(0.f).margin(1e-5));
    REQUIRE(*StepDirection({10.f, 10.f, 0.f}, {10.f, 5.f, 0.f}, cQuarter) == Approx(0.25f));

    // Height is left out; no step across the ground, or a broken one, has no direction.
    REQUIRE(*StepDirection({}, {0.f, -3.f, 40.f}, 0.f) == Approx(0.5f));
    REQUIRE_FALSE(StepDirection({}, {0.f, 0.f, 10.f}, 0.f));
    REQUIRE_FALSE(StepDirection({}, {std::numeric_limits<float>::quiet_NaN(), 1.f, 0.f}, 0.f));
    REQUIRE_FALSE(StepDirection({}, {1.f, 1.f, 0.f}, std::numeric_limits<float>::infinity()));

    // Always within 0 to 1, whatever the heading's turns.
    const float cWound = *StepDirection({}, {5.f, 0.f, 0.f}, 3.f * kFullTurn);
    REQUIRE(cWound >= 0.f);
    REQUIRE(cWound < 1.f);
    REQUIRE(cWound == Approx(0.25f));
}

TEST_CASE("The shown step direction eases the short way round", "[stride_match]")
{
    // None yet: taken whole.
    REQUIRE(EaseDirection(-1.f, 0.25f, 10.0) == 0.25f);
    // No time: unchanged; a broken sample: unchanged.
    REQUIRE(EaseDirection(0.25f, 0.5f, 0.0) == 0.25f);
    REQUIRE(EaseDirection(0.25f, std::numeric_limits<float>::quiet_NaN(), 10.0) == 0.25f);
    REQUIRE(EaseDirection(0.25f, 1.5f, 10.0) == 0.25f);

    // Part of the way after a short frame, nearly all of it after a long one.
    const float cPart = EaseDirection(0.f, 0.25f, 16.0);
    REQUIRE(cPart > 0.f);
    REQUIRE(cPart < 0.25f);
    REQUIRE(EaseDirection(0.f, 0.25f, 2000.0) == Approx(0.25f).margin(1e-3));

    // From just left of forward to just right of it goes through forward, not round the back.
    const float cAcross = EaseDirection(0.95f, 0.05f, 60.0);
    REQUIRE((cAcross > 0.95f || cAcross < 0.05f));
}

TEST_CASE("A step direction becomes the process's radians", "[stride_match]")
{
    REQUIRE(DirectionRadians(0.f) == Approx(0.f));
    REQUIRE(DirectionRadians(0.25f) == Approx(kFullTurn / 4.f));
    REQUIRE(DirectionRadians(0.75f) == Approx(-kFullTurn / 4.f));
    REQUIRE(std::fabs(DirectionRadians(0.5f)) == Approx(kFullTurn / 2.f));
}

TEST_CASE("Every speed band plays the gait whose pace it is: walk and run at the shown speed, a sprint only at a sprint's pace", "[stride_match]")
{
    // Walk, run and a VR roomscale-assisted pace: the legs are driven at the speed shown, which the game picks the clip and its rate
    // from; still below kStillSpeed.
    for (const float cShown : {0.f, 5.f, 60.f, 120.f, 180.f, 260.f, 330.f, 369.f})
    {
        AnimationVariables variables;
        variables.Floats = {0.f, 0.f, 0.f, 0.f, 999.f};
        const float cLegs = LegSpeed(cShown, 999.f);
        Drive(variables, 4, 2, cLegs);
        REQUIRE(variables.Floats[4] == (cShown >= kStillSpeed ? cShown : 0.f));
        REQUIRE(variables.Floats[2] == variables.Floats[4]);
    }

    // A sprint below a sprint clip's pace is ended (its feet would run ahead); at or above it, left alone.
    for (const float cShown : {150.f, 240.f, 300.f, kSprintFloor - 1.f})
    {
        SprintWatch watch;
        REQUIRE_FALSE(StopsSprint(watch, true, cShown, 0.0));
        REQUIRE(StopsSprint(watch, true, cShown, kSprintSlowMs));
    }
    for (const float cShown : {kSprintFloor, 450.f, 500.f, 650.f})
    {
        SprintWatch watch;
        for (double now = 0.0; now < 2000.0; now += 10.0)
            REQUIRE_FALSE(StopsSprint(watch, true, cShown, now));
    }

    // Not sprinting: started only at a sprint's pace, held for kSprintSlowMs; a run, even a fast one, is left a run.
    for (const float cShown : {150.f, 300.f, kSprintFloor, kSprintStartPace - 1.f})
    {
        SprintWatch watch;
        for (double now = 0.0; now < 2000.0; now += 10.0)
            REQUIRE_FALSE(StartsSprint(watch, false, cShown, now));
    }
    for (const float cShown : {kSprintStartPace, 500.f, 650.f})
    {
        SprintWatch watch;
        REQUIRE_FALSE(StartsSprint(watch, false, cShown, 0.0));
        REQUIRE_FALSE(StartsSprint(watch, false, cShown, kSprintSlowMs - 1.0));
        REQUIRE(StartsSprint(watch, false, cShown, kSprintSlowMs));
    }
}

TEST_CASE("A sprint cut by a slow spell comes back at a sprint's pace, once per climb", "[stride_match]")
{
    SprintWatch watch;
    // Sprinting at 500, a turn slows it to 300: ended.
    for (double now = 0.0; now < 1000.0; now += 10.0)
        REQUIRE_FALSE(StopsSprint(watch, true, 500.f, now));
    bool stopped = false;
    for (double now = 1000.0; now < 2000.0 && !stopped; now += 10.0)
        stopped = StopsSprint(watch, true, 300.f, now);
    REQUIRE(stopped);

    // Back at 500 with its graph running: started once, and not again while it stays at that pace (a refused start is not repeated).
    int starts = 0;
    for (double now = 2000.0; now < 5000.0; now += 10.0)
        starts += StartsSprint(watch, false, 500.f, now);
    REQUIRE(starts == 1);

    // A short dip into the gap between run and sprint pace is no new climb; one below a run's pace is.
    for (double now = 5000.0; now < 5300.0; now += 10.0)
        REQUIRE_FALSE(StartsSprint(watch, false, 400.f, now));
    for (double now = 5300.0; now < 6000.0; now += 10.0)
        REQUIRE_FALSE(StartsSprint(watch, false, 500.f, now));
    REQUIRE_FALSE(StartsSprint(watch, false, 300.f, 6000.0));
    starts = 0;
    for (double now = 6010.0; now < 8000.0; now += 10.0)
        starts += StartsSprint(watch, false, 500.f, now);
    REQUIRE(starts == 1);

    // Sprinting already: never; a speed that is not a number: never.
    SprintWatch sprinting;
    for (double now = 0.0; now < 2000.0; now += 10.0)
    {
        REQUIRE_FALSE(StartsSprint(sprinting, true, 500.f, now));
        REQUIRE_FALSE(StartsSprint(sprinting, false, std::numeric_limits<float>::quiet_NaN(), now));
    }
}
