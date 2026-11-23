#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>

#include <catch2/catch.hpp>

#include <Systems/RemoteSmoothing.h>

#include <cmath>
#include <limits>

using namespace RemoteSmoothing;

namespace
{
// How far apart two headings are, as a share of a turn, the short way round.
float HeadingGap(const float aLeft, const float aRight)
{
    return std::abs(std::remainder(aLeft - aRight, 1.f));
}
} // namespace

TEST_CASE("A copy that changes hands glides to its new owner's place in about 150 ms", "[remote_smoothing]")
{
    REQUIRE(HandOffWeight(0.0) == 1.f);
    REQUIRE(HandOffWeight(-5.0) == 1.f);
    REQUIRE(HandOffWeight(std::nan("")) == 1.f);
    REQUIRE(HandOffWeight(kHandOffBlendMs / 2.0) == Approx(0.5f));
    REQUIRE(HandOffWeight(kHandOffBlendMs) == 0.f);
    REQUIRE(HandOffWeight(10'000.0) == 0.f);

    // A running NPC's hand-off gap (20-90 units, as seen in a review): no frame moves further than the fade allows.
    // Smoothstep's steepest slope is 1.5 / kHandOffBlendMs, so at 60 frames a second a 90-unit gap moves at most 15 units a frame.
    const glm::vec3 cGap = HandOffGap({90.f, 0.f, 0.f}, {0.f, 0.f, 0.f});
    REQUIRE(cGap.x == Approx(90.f));
    constexpr double cFrame = 1000.0 / 60.0;
    float previous = HandOffWeight(0.0) * cGap.x;
    float largest = 0.f;
    for (double since = cFrame; since <= kHandOffBlendMs + cFrame; since += cFrame)
    {
        const float cShown = HandOffWeight(since) * cGap.x;
        REQUIRE(cShown <= previous);
        largest = std::max(largest, previous - cShown);
        previous = cShown;
    }
    REQUIRE(previous == 0.f);
    REQUIRE(largest <= static_cast<float>(90.0 * 1.5 * cFrame / kHandOffBlendMs) + 0.01f);
}

TEST_CASE("A hand-off gap too wide to glide snaps, as the movement's corrections do", "[remote_smoothing]")
{
    REQUIRE(glm::length(HandOffGap({0.f, 0.f, 0.f}, {0.f, kMaxHandOffGap, 0.f})) == Approx(kMaxHandOffGap));
    REQUIRE(HandOffGap({0.f, 0.f, 0.f}, {0.f, kMaxHandOffGap + 1.f, 0.f}) == glm::vec3{});
    const float cNan = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(HandOffGap({cNan, 0.f, 0.f}, {0.f, 0.f, 0.f}) == glm::vec3{});
}

TEST_CASE("Only the legs' speed, turn and direction are blended, by name", "[remote_smoothing]")
{
    REQUIRE(BlendOf("Speed") == FloatBlend::Linear);
    REQUIRE(BlendOf("SpeedSampled") == FloatBlend::Linear);
    REQUIRE(BlendOf("HorseSpeedSampled") == FloatBlend::Linear);
    REQUIRE(BlendOf("TurnDelta") == FloatBlend::Linear);
    REQUIRE(BlendOf("Direction") == FloatBlend::Turn);
    // Weights and switches the graph steps through: the newer point's.
    REQUIRE(BlendOf("CastBlend") == FloatBlend::Newer);
    REQUIRE(BlendOf("weapAdj") == FloatBlend::Newer);
    REQUIRE(BlendOf("Pitch") == FloatBlend::Newer);
    REQUIRE(BlendOf("") == FloatBlend::Newer);
}

TEST_CASE("Leg floats are blended between two points, a heading the short way round", "[remote_smoothing]")
{
    // Speed from a stand to a run, as the body speeds up, and a continuous 0 to 1 ramp: never taken for a switch.
    REQUIRE(BlendFloat(FloatBlend::Linear, 0.f, 300.f, 0.25f) == Approx(75.f));
    REQUIRE(BlendFloat(FloatBlend::Linear, 0.f, 1.f, 0.5f) == Approx(0.5f));
    REQUIRE(BlendFloat(FloatBlend::Linear, 0.1f, 0.9f, 0.25f) == Approx(0.3f));
    // A turn either way.
    REQUIRE(BlendFloat(FloatBlend::Linear, -0.2f, 0.2f, 0.5f) == Approx(0.f));
    // A heading blends the short way, across the seam, and stays within a turn.
    REQUIRE(BlendFloat(FloatBlend::Turn, 0.2f, 0.3f, 0.5f) == Approx(0.25f));
    REQUIRE(HeadingGap(BlendFloat(FloatBlend::Turn, 0.95f, 0.05f, 0.25f), 0.975f) < 1e-5f);
    REQUIRE(HeadingGap(BlendFloat(FloatBlend::Turn, 0.95f, 0.05f, 0.75f), 0.025f) < 1e-5f);
    REQUIRE(HeadingGap(BlendFloat(FloatBlend::Turn, 0.05f, 0.95f, 0.5f), 0.f) < 1e-5f);
    for (float delta = 0.f; delta <= 1.f; delta += 0.125f)
    {
        const float cHeading = BlendFloat(FloatBlend::Turn, 0.9f, 0.1f, delta);
        REQUIRE(cHeading >= 0.f);
        REQUIRE(cHeading < 1.f);
    }
    REQUIRE(HeadingGap(BlendFloat(FloatBlend::Turn, 0.f, 1.f, 0.5f), 0.f) < 1e-5f);
    // Not a share of a turn: not wrapped, the newer point's.
    REQUIRE(BlendFloat(FloatBlend::Turn, 10.f, 350.f, 0.5f) == 350.f);
    // A switch, or not a number.
    REQUIRE(BlendFloat(FloatBlend::Newer, 0.f, 1.f, 0.5f) == 1.f);
    REQUIRE(BlendFloat(FloatBlend::Linear, std::numeric_limits<float>::quiet_NaN(), 2.f, 0.5f) == 2.f);
}

TEST_CASE("Blended graph variables keep the newer point's booleans, integers and unknown floats", "[remote_smoothing]")
{
    AnimationVariables older;
    older.Booleans = {false, true};
    older.Integers = {1, 2};
    older.Floats = {0.f, 0.9f, 0.f, 100.f};
    AnimationVariables newer;
    newer.Booleans = {true, false};
    newer.Integers = {3, 4};
    newer.Floats = {200.f, 0.1f, 1.f, 100.f};
    const TiltedPhoques::Vector<FloatBlend> cKinds{FloatBlend::Linear, FloatBlend::Turn, FloatBlend::Newer, FloatBlend::Linear};

    AnimationVariables out;
    BlendVariables(older, newer, 0.5f, cKinds, out);
    REQUIRE(out.Booleans == newer.Booleans);
    REQUIRE(out.Integers == newer.Integers);
    REQUIRE(out.Floats[0] == Approx(100.f));
    REQUIRE(HeadingGap(out.Floats[1], 0.f) < 1e-5f);
    REQUIRE(out.Floats[2] == 1.f);
    REQUIRE(out.Floats[3] == Approx(100.f));

    BlendVariables(older, newer, 1.f, cKinds, out);
    REQUIRE(out == newer);
    BlendVariables(older, newer, 7.f, cKinds, out);
    REQUIRE(out == newer);
    BlendVariables(older, newer, std::nanf(""), cKinds, out);
    REQUIRE(out == newer);

    // Another graph's list, or one the copy's graph does not match: the newer point's whole.
    AnimationVariables other = older;
    other.Floats.push_back(1.f);
    BlendVariables(other, newer, 0.5f, cKinds, out);
    REQUIRE(out == newer);
    const TiltedPhoques::Vector<FloatBlend> cShort{FloatBlend::Linear};
    BlendVariables(older, newer, 0.5f, cShort, out);
    REQUIRE(out == newer);
}

TEST_CASE("A burst plays up to three of one owner frame's actions a frame, a spawn's history one a frame", "[remote_smoothing]")
{
    REQUIRE(PlayAnotherAction(1, true, true, true, false));
    REQUIRE(PlayAnotherAction(2, true, true, true, false));
    REQUIRE_FALSE(PlayAnotherAction(kMaxActionsPerFrame, true, true, true, false));
    // Nothing more due, the last one was kept back (its graph was not ready), or a replay.
    REQUIRE_FALSE(PlayAnotherAction(1, true, false, true, false));
    REQUIRE_FALSE(PlayAnotherAction(1, false, true, true, false));
    REQUIRE_FALSE(PlayAnotherAction(1, true, true, true, true));
}

TEST_CASE("Actions from later owner frames keep a frame between them on the copy", "[remote_smoothing]")
{
    // A late block, bash and release (a VR bash sends its steps a frame apart), or a draw then an attack: all due at once,
    // each from its own frame. Each waits for the copy's next frame, as on the owner's game.
    struct Action
    {
        uint64_t Tick;
    };
    const auto cFrames = [](const TiltedPhoques::Vector<Action>& acDue)
    {
        uint32_t frames = 0;
        size_t next = 0;
        while (next < acDue.size())
        {
            ++frames;
            for (uint32_t played = 0;;)
            {
                const uint64_t cPlayed = acDue[next++].Tick;
                ++played;
                const bool cNextDue = next < acDue.size();
                if (!PlayAnotherAction(played, true, cNextDue, cNextDue && acDue[next].Tick == cPlayed, false))
                    break;
            }
        }
        return frames;
    };
    REQUIRE(cFrames({{1000}, {1011}, {1400}}) == 3);
    REQUIRE(cFrames({{1000}, {1001}}) == 2);
    // One owner frame's draw, swing and block: one copy frame. A fourth waits for the cap.
    REQUIRE(cFrames({{1000}, {1000}, {1000}}) == 1);
    REQUIRE(cFrames({{1000}, {1000}, {1000}, {1000}}) == 2);
    REQUIRE(cFrames({{1000}, {1000}, {1022}}) == 2);
}
