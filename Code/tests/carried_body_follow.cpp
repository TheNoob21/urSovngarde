#include <catch2/catch.hpp>

#include <Games/Skyrim/CarriedBodyFollow.h>

#include <limits>

using namespace CarriedBodyFollow;
using namespace std::chrono_literals;

namespace
{
constexpr uint64_t kSkeleton = 0x5EED;

BodyNow Carried() noexcept
{
    BodyNow body{};
    body.RemoteNpc = true;
    body.Dead = true;
    body.OwnerBonesMove = true;
    return body;
}

// Begins a frame past the blend, so speeds are given.
Follow Blended(const Clock::time_point aStart, const size_t aBodies)
{
    Follow follow;
    for (auto at = aStart; at <= aStart + kBlendIn; at += 50ms)
    {
        (void)follow.Begin(at, aBodies, kSkeleton);
        for (size_t i = 0; i < aBodies; ++i)
            (void)follow.Step(i, {});
    }
    return follow;
}
} // namespace

TEST_CASE("a carried body is followed only while its owner's bones move it", "[carry_follow]")
{
    CHECK(Follows(Carried()));

    BodyNow knocked = Carried();
    knocked.Dead = false;
    knocked.KnockRagdoll = true;
    CHECK(Follows(knocked));

    // The owner let go, or its next pose is late: the body is not set and keeps its speed.
    BodyNow body = Carried();
    body.OwnerBonesMove = false;
    CHECK_FALSE(Follows(body));

    // Standing (got up), dying, kneeling in bleedout, a player's copy, held here or loading: never.
    body = Carried();
    body.Dead = false;
    CHECK_FALSE(Follows(body));
    body = Carried();
    body.Dying = true;
    CHECK_FALSE(Follows(body));
    body = knocked;
    body.BleedingOut = true;
    CHECK_FALSE(Follows(body));
    body = Carried();
    body.RemoteNpc = false;
    CHECK_FALSE(Follows(body));
    body = Carried();
    body.HeldHere = true;
    CHECK_FALSE(Follows(body));
    body = Carried();
    body.Loading = true;
    CHECK_FALSE(Follows(body));
}

TEST_CASE("at most four carried bodies are followed, those already followed first, then the nearest", "[carry_follow]")
{
    std::vector<Candidate> candidates{{1, 900.f, false}, {2, 100.f, false}, {3, 300.f, false}, {4, 200.f, false}, {5, 50.f, false}, {6, 700.f, false}};
    const auto chosen = Choose(candidates);
    REQUIRE(chosen.size() == kMaxFollowed);
    CHECK(chosen == std::vector<uint32_t>{5, 2, 4, 3});

    // A body followed already keeps its place against a nearer new one.
    candidates[0].Followed = true;
    CHECK(Choose(candidates) == std::vector<uint32_t>{1, 5, 2, 4});

    // More followed than the bound: the nearest of them.
    for (auto& candidate : candidates)
        candidate.Followed = true;
    CHECK(Choose(candidates) == std::vector<uint32_t>{5, 2, 4, 3});

    // Fewer than the bound: all; none: none; a repeated id counts once.
    CHECK(Choose({{7, 10.f, false}, {7, 10.f, false}}) == std::vector<uint32_t>{7});
    CHECK(Choose({}).empty());
    CHECK(Choose(candidates, 1) == std::vector<uint32_t>{5});
}

TEST_CASE("a followed body blends in from its ragdoll, then moves with its owner's speed", "[carry_follow]")
{
    const Clock::time_point t0{std::chrono::seconds(100)};
    Follow follow;
    CHECK(follow.Lapsed(t0));

    // The first frame moves a little, never nothing.
    CHECK(follow.Begin(t0, 2, kSkeleton) == Approx(0.1f));
    CHECK(follow.Step(0, {0.f, 0.f, 0.f}) == glm::vec3{});
    CHECK(follow.Step(1, {10.f, 0.f, 0.f}) == glm::vec3{});
    CHECK_FALSE(follow.Lapsed(t0 + 100ms));

    // Blending in: no speed, so a body let go now drops instead of being flung by the blend.
    CHECK(follow.Begin(t0 + 75ms, 2, kSkeleton) == Approx(0.5f));
    CHECK(follow.Step(0, {15.f, 0.f, 0.f}) == glm::vec3{});
    (void)follow.Step(1, {10.f, 0.f, 0.f});

    // Blended: 30 units in 75 ms is 400 units a second.
    CHECK(follow.Begin(t0 + 150ms, 2, kSkeleton) == Approx(1.f));
    CHECK(follow.Step(0, {45.f, 0.f, 0.f}).x == Approx(400.f));
    CHECK(follow.Step(1, {10.f, 0.f, 0.f}) == glm::vec3{});
    CHECK(follow.Begin(t0 + 200ms, 2, kSkeleton) == Approx(1.f));
}

TEST_CASE("a followed body keeps the root offset it had when the follow began", "[carry_follow]")
{
    const Clock::time_point t0{std::chrono::seconds(100)};
    Follow follow;
    (void)follow.Begin(t0, 1, kSkeleton);
    CHECK(follow.RootOffset({10.f, 0.f, 30.f}, {0.f, 0.f, 0.f}) == glm::vec3{10.f, 0.f, 30.f});
    // The 3D root lags or leads its ragdoll later on: the first offset still holds, so the body is not pushed on.
    (void)follow.Begin(t0 + 16ms, 1, kSkeleton);
    CHECK(follow.RootOffset({50.f, 0.f, 30.f}, {0.f, 0.f, 0.f}) == glm::vec3{10.f, 0.f, 30.f});

    // A new follow after a lapse reads it again; a bad reading is none.
    (void)follow.Begin(t0 + 2s, 1, kSkeleton);
    CHECK(follow.RootOffset({500.f, 0.f, 0.f}, {0.f, 0.f, 0.f}) == glm::vec3{});
    Follow other;
    (void)other.Begin(t0, 1, kSkeleton);
    CHECK(other.RootOffset({std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f}, {}) == glm::vec3{});
}

TEST_CASE("a followed body's speed is bounded and restarts on a new skeleton", "[carry_follow]")
{
    const Clock::time_point t0{std::chrono::seconds(100)};
    Follow follow = Blended(t0, 1);
    const auto t1 = t0 + kBlendIn;

    // 1000 units in 10 ms is a bad read: capped at kMaxSpeed, same direction.
    (void)follow.Begin(t1 + 10ms, 1, kSkeleton);
    const glm::vec3 cFast = follow.Step(0, {0.f, 1000.f, 0.f});
    CHECK(glm::length(cFast) == Approx(kMaxSpeed));
    CHECK(cFast.y > 0.f);

    // A ragdoll with another number of bodies: no speed from the old ones.
    (void)follow.Begin(t1 + 20ms, 3, kSkeleton);
    CHECK(follow.Step(0, {0.f, 1010.f, 0.f}) == glm::vec3{});
    // More bodies than kept: only those kept get speeds.
    (void)follow.Begin(t1 + 30ms, kMaxBodies + 5, kSkeleton);
    CHECK(follow.Step(kMaxBodies, {1.f, 1.f, 1.f}) == glm::vec3{});

    // Not a number: no speed, and the next good step after it none either.
    Follow other = Blended(t0, 1);
    (void)other.Begin(t1 + 10ms, 1, kSkeleton);
    CHECK(other.Step(0, {std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f}) == glm::vec3{});
    (void)other.Begin(t1 + 20ms, 1, kSkeleton);
    CHECK(other.Step(0, {5.f, 0.f, 0.f}) == glm::vec3{});
    (void)other.Begin(t1 + 30ms, 1, kSkeleton);
    CHECK(other.Step(0, {6.f, 0.f, 0.f}).x == Approx(100.f));
}

TEST_CASE("a carried body not set for a while is left to its ragdoll, and blends in afresh when taken up again", "[carry_follow]")
{
    const Clock::time_point t0{std::chrono::seconds(100)};
    Follow follow = Blended(t0, 1);
    const auto cLast = t0 + kBlendIn;

    // Set again within kLapse: still followed.
    CHECK_FALSE(follow.Lapsed(cLast + kLapse));
    // Not set past kLapse: let go, with the speed it was last given.
    CHECK(follow.Lapsed(cLast + kLapse + 1ms));

    // Taken up again: a new blend, and no speed from before the gap.
    CHECK(follow.Begin(cLast + 2s, 1, kSkeleton) == Approx(0.1f));
    CHECK(follow.Step(0, {500.f, 0.f, 0.f}) == glm::vec3{});
}

TEST_CASE("a follow asked for is set a frame later only if its pose is fresh and nothing changed meanwhile", "[carry_follow]")
{
    const Clock::time_point t0{std::chrono::seconds(100)};
    const Asked cAsked{t0, 0x100, 0x3C, 0x3C};

    // The next frame: set.
    CHECK(StillAsked(cAsked, Asked{t0 + 16ms, 0x100, 0x3C, 0x3C}));
    CHECK(StillAsked(cAsked, Asked{t0 + kMaxAskAge, 0x100, 0x3C, 0x3C}));

    // A delayed frame: its pose is stale.
    CHECK_FALSE(StillAsked(cAsked, Asked{t0 + kMaxAskAge + 1ms, 0x100, 0x3C, 0x3C}));
    // The copy was made again as another reference, it went to another cell, or the player did.
    CHECK_FALSE(StillAsked(cAsked, Asked{t0 + 16ms, 0x200, 0x3C, 0x3C}));
    CHECK_FALSE(StillAsked(cAsked, Asked{t0 + 16ms, 0x100, 0x3D, 0x3C}));
    CHECK_FALSE(StillAsked(cAsked, Asked{t0 + 16ms, 0x100, 0x3C, 0x3D}));
    // No reference when asked, or a clock that went back: never.
    CHECK_FALSE(StillAsked(Asked{t0, 0, 0x3C, 0x3C}, Asked{t0 + 16ms, 0, 0x3C, 0x3C}));
    CHECK_FALSE(StillAsked(cAsked, Asked{t0 - 1ms, 0x100, 0x3C, 0x3C}));
}

TEST_CASE("a skeleton made again with as many bodies gives no speed from the old one's bodies", "[carry_follow]")
{
    int bodies[4]{};
    const void* const cOld[2]{&bodies[0], &bodies[1]};
    const void* const cNew[2]{&bodies[2], &bodies[3]};
    const void* const cSwapped[2]{&bodies[1], &bodies[0]};
    const uint64_t cOldSkeleton = SkeletonOf(cOld, 2);
    const uint64_t cNewSkeleton = SkeletonOf(cNew, 2);
    CHECK(cOldSkeleton == SkeletonOf(cOld, 2));
    CHECK(cOldSkeleton != cNewSkeleton);
    CHECK(cOldSkeleton != SkeletonOf(cSwapped, 2));

    const Clock::time_point t0{std::chrono::seconds(100)};
    Follow follow;
    for (auto at = t0; at <= t0 + kBlendIn; at += 50ms)
    {
        (void)follow.Begin(at, 2, cOldSkeleton);
        (void)follow.Step(0, {});
        (void)follow.Step(1, {});
    }
    const auto t1 = t0 + kBlendIn;

    // The old skeleton moving: 10 units in 10 ms is 1000 units a second.
    (void)follow.Begin(t1 + 10ms, 2, cOldSkeleton);
    CHECK(follow.Step(0, {10.f, 0.f, 0.f}).x == Approx(1000.f));
    (void)follow.Step(1, {10.f, 0.f, 0.f});

    // Made again, two bodies as before, lying elsewhere: no speed from where the old ones were set.
    (void)follow.Begin(t1 + 20ms, 2, cNewSkeleton);
    CHECK(follow.Step(0, {500.f, 0.f, 0.f}) == glm::vec3{});
    CHECK(follow.Step(1, {500.f, 0.f, 0.f}) == glm::vec3{});
    // The next frame of the new skeleton has its own speed again.
    (void)follow.Begin(t1 + 30ms, 2, cNewSkeleton);
    CHECK(follow.Step(0, {505.f, 0.f, 0.f}).x == Approx(500.f));
}
