#include <catch2/catch.hpp>

#include <Systems/BodySolver.h>

// A VR player's pose is always sent with VRIK, and without it only while nothing is drawn.

using namespace BodySolver;
using namespace std::chrono_literals;

TEST_CASE("Body solver: asked at once, then every few seconds", "[body_solver]")
{
    Watch watch;
    const Clock::time_point cStart{};
    CHECK(watch.Due(cStart));
    CHECK_FALSE(watch.Loaded());

    watch.Take(true, cStart);
    CHECK(watch.Loaded());
    CHECK_FALSE(watch.Due(cStart));
    CHECK_FALSE(watch.Due(cStart + kAskEvery - 1ms));
    CHECK(watch.Due(cStart + kAskEvery));
}

TEST_CASE("Body solver: said when missing at first and at every change, not on every ask", "[body_solver]")
{
    const Clock::time_point cStart{};

    // Found at first: nothing to say, the pose goes out as before.
    Watch found;
    CHECK(found.Take(true, cStart) == Watch::Said::kNothing);
    CHECK(found.Take(true, cStart + kAskEvery) == Watch::Said::kNothing);
    CHECK(found.Take(false, cStart + 2 * kAskEvery) == Watch::Said::kChanged);
    CHECK_FALSE(found.Loaded());

    // Missing at first: said once, and not again while it stays missing.
    Watch missing;
    CHECK(missing.Take(false, cStart) == Watch::Said::kChanged);
    CHECK(missing.Take(false, cStart + kAskEvery) == Watch::Said::kNothing);
    CHECK(missing.Take(false, cStart + 2 * kAskEvery) == Watch::Said::kNothing);
    CHECK(missing.Take(true, cStart + 3 * kAskEvery) == Watch::Said::kChanged);
    CHECK(missing.Loaded());
}

TEST_CASE("Body solver: without VRIK the pose is sent only while nothing is drawn", "[body_solver]")
{
    // With VRIK, in every weapon state.
    for (uint32_t state = 0; state < 8; ++state)
        CHECK(SendsPose(true, state));

    // Without it, sheathed only: from the wish to draw until the weapon is fully away the copy plays its own stance.
    CHECK(SendsPose(false, 0));
    for (uint32_t state = 1; state <= 5; ++state)
        CHECK_FALSE(SendsPose(false, state));
}
