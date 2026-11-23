#include <catch2/catch.hpp>

#include <cmath>
#include <limits>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Systems/HandReach.h>

namespace
{
void NearPosition(const glm::vec3& acA, const glm::vec3& acB, const float aMargin)
{
    INFO("(" << acA.x << ", " << acA.y << ", " << acA.z << ") vs (" << acB.x << ", " << acB.y << ", " << acB.z << ")");
    CHECK(glm::distance(acA, acB) <= aMargin);
}
} // namespace

namespace
{
// A right arm reaching forward, its clavicle out to the side: clavicle 0, upper arm 1 (shoulder), forearm 2 (elbow),
// hand 3 (wrist).
BodyPosePresentation::Output ArmWithClavicle()
{
    BodyPosePresentation::Output out;
    out.Count = 4;
    out.world[0].Translation = {0.f, 0.f, 100.f};
    out.world[1].Translation = {15.f, 0.f, 100.f};
    out.world[2].Translation = {22.f, 20.f, 95.f};
    out.world[3].Translation = {16.f, 34.f, 92.f};
    for (size_t i = 0; i < out.Count; ++i)
        out.world[i].Rotation = glm::angleAxis(.3f * static_cast<float>(i), glm::normalize(glm::vec3{1.f, 2.f, 3.f}));
    return out;
}

std::array<int, BodyPosePresentation::kMaxNodes> ClavicleParents()
{
    std::array<int, BodyPosePresentation::kMaxNodes> parent{};
    parent.fill(BodyPosePresentation::kMissing);
    parent[1] = 0;
    parent[2] = 1;
    parent[3] = 2;
    return parent;
}
} // namespace

TEST_CASE("a hand beyond the arm's reach is reached by turning the clavicle first", "[handreach]")
{
    using namespace BodyPosePresentation;
    const auto parent = ClavicleParents();
    const auto before = ArmWithClavicle();
    const float upper = glm::distance(before.world[1].Translation, before.world[2].Translation);
    const float fore = glm::distance(before.world[2].Translation, before.world[3].Translation);
    const glm::vec3 shoulder = before.world[1].Translation;
    const glm::vec3 direction = glm::normalize(before.world[3].Translation - shoulder);
    // 4 units beyond the straight arm, within the fine-tuning cap.
    const glm::vec3 target = shoulder + (upper + fore + 4.f) * direction;
    REQUIRE(glm::distance(target, before.world[3].Translation) <= kReachMaxCorrection);
    const glm::quat cHandWanted = before.world[3].Rotation;

    // Without the clavicle the arm stops 4.5 units short (the gap the presenter keeps today).
    auto alone = before;
    const auto cAlone = ReachHand(alone, parent, glm::quat{1.f, 0.f, 0.f, 0.f}, 1, 2, 3, target, cHandWanted);
    REQUIRE(cAlone.Outcome == Reach::Applied);
    CHECK(cAlone.GapAfter == Approx(4.5f).margin(1e-2));

    auto out = before;
    const float cTurn = HandReach::TurnClavicle(out, parent, 0, 1, 2, 3, target);
    CHECK(cTurn > 0.f);
    CHECK(cTurn <= HandReach::kMaxTurn);
    // The clavicle turns about its own joint; the arm keeps its lengths.
    NearPosition(out.world[0].Translation, before.world[0].Translation, 1e-6f);
    CHECK(glm::distance(out.world[0].Translation, out.world[1].Translation) == Approx(15.f).margin(1e-3));
    CHECK(glm::distance(out.world[1].Translation, out.world[2].Translation) == Approx(upper).margin(1e-3));
    CHECK(glm::distance(out.world[2].Translation, out.world[3].Translation) == Approx(fore).margin(1e-3));
    // Just enough: the target is now exactly as far as the arm reaches.
    CHECK(glm::distance(target, out.world[1].Translation) == Approx(upper + fore - HandReach::kStraightSlack).margin(1e-2));

    const auto cReached = ReachHand(out, parent, glm::quat{1.f, 0.f, 0.f, 0.f}, 1, 2, 3, target, cHandWanted);
    CHECK(cReached.Outcome == Reach::Applied);
    CHECK(cReached.GapAfter < 0.05f);
}

TEST_CASE("the clavicle is left alone within reach, for big gaps and bad bones, and turns at most its limit", "[handreach]")
{
    using namespace BodyPosePresentation;
    const auto parent = ClavicleParents();
    const auto before = ArmWithClavicle();
    const auto same = [&before](const Output& acOut)
    {
        for (size_t i = 0; i < acOut.Count; ++i)
            if (!(acOut.world[i].Translation == before.world[i].Translation) || !(acOut.world[i].Rotation == before.world[i].Rotation))
                return false;
        return true;
    };

    auto out = before;
    CHECK(HandReach::TurnClavicle(out, parent, 0, 1, 2, 3, before.world[3].Translation + glm::vec3{1.f, 1.f, 0.f}) == 0.f);
    CHECK(HandReach::TurnClavicle(out, parent, 0, 1, 2, 3, before.world[3].Translation + glm::vec3{0.f, 30.f, 0.f}) == 0.f);
    CHECK(HandReach::TurnClavicle(out, parent, 1, 0, 2, 3, before.world[3].Translation + glm::vec3{0.f, 5.f, 0.f}) == 0.f);
    CHECK(HandReach::TurnClavicle(out, parent, 0, 1, 2, 9, before.world[3].Translation) == 0.f);
    CHECK(HandReach::TurnClavicle(out, parent, 0, 1, 2, 3, glm::vec3{std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f}) == 0.f);
    CHECK(same(out));

    // Far beyond reach but within the cap, with a small limit: the limit, and nearer than before.
    const glm::vec3 shoulder = before.world[1].Translation;
    const glm::vec3 direction = glm::normalize(before.world[3].Translation - shoulder);
    const float upper = glm::distance(before.world[1].Translation, before.world[2].Translation);
    const float fore = glm::distance(before.world[2].Translation, before.world[3].Translation);
    const glm::vec3 target = shoulder + (upper + fore + 8.f) * direction;
    REQUIRE(glm::distance(target, before.world[3].Translation) <= kReachMaxCorrection);
    constexpr float cLimit = 0.05f;
    CHECK(HandReach::TurnClavicle(out, parent, 0, 1, 2, 3, target, kReachMaxCorrection, cLimit) == Approx(cLimit));
    CHECK(glm::distance(target, out.world[1].Translation) < glm::distance(target, before.world[1].Translation));
}

TEST_CASE("the clavicle turn never swings the wrist out of the arm's fine-tuning range", "[handreach]")
{
    using namespace BodyPosePresentation;
    const auto parent = ClavicleParents();
    const auto before = ArmWithClavicle();
    const glm::vec3 shoulder = before.world[1].Translation;
    const float upper = glm::distance(before.world[1].Translation, before.world[2].Translation);
    const float fore = glm::distance(before.world[2].Translation, before.world[3].Translation);
    const glm::vec3 direction = glm::normalize(before.world[3].Translation - shoulder);
    // 8 units beyond the straight arm: the full turn would carry the wrist further than the 15-unit range.
    const glm::vec3 target = shoulder + (upper + fore + 8.f) * direction;
    REQUIRE(glm::distance(target, before.world[3].Translation) <= kReachMaxCorrection);

    auto alone = before;
    const auto cAlone = ReachHand(alone, parent, glm::quat{1.f, 0.f, 0.f, 0.f}, 1, 2, 3, target, before.world[3].Rotation);
    REQUIRE(cAlone.Outcome == Reach::Applied);

    auto out = before;
    HandReach::TurnClavicle(out, parent, 0, 1, 2, 3, target);
    CHECK(glm::distance(target, out.world[3].Translation) <= kReachMaxCorrection);
    const auto cTurned = ReachHand(out, parent, glm::quat{1.f, 0.f, 0.f, 0.f}, 1, 2, 3, target, before.world[3].Rotation);
    CHECK(cTurned.Outcome == Reach::Applied);
    CHECK(cTurned.GapAfter <= cAlone.GapAfter + 1e-3f);
}
