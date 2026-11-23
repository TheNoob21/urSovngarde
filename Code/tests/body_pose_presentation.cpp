#include <catch2/catch.hpp>
#include <Systems/BodyPosePresentation.h>
#include <glm/gtc/constants.hpp>
#include <limits>

using namespace BodyPosePresentation;
namespace
{
glm::quat Yaw(float aRadians)
{
    return glm::angleAxis(aRadians, glm::vec3{0.f, 0.f, 1.f});
}
void Near(const glm::vec3& acActual, const glm::vec3& acExpected)
{
    CHECK(acActual.x == Approx(acExpected.x).margin(0.0001));
    CHECK(acActual.y == Approx(acExpected.y).margin(0.0001));
    CHECK(acActual.z == Approx(acExpected.z).margin(0.0001));
}
void Identity(const glm::quat& acQ)
{
    CHECK(std::fabs(glm::dot(acQ, glm::quat{1.f, 0.f, 0.f, 0.f})) == Approx(1.f).margin(0.0001));
}
Input Nested()
{
    Input in;
    in.Count = 5;
    in.HasData = true;
    in.HasLegs = true;
    // Unsorted: head 0 -> spine 3 -> pelvis 4; hand 1 -> spine; skin 2 -> pelvis.
    in.Parent[0] = 3;
    in.Parent[1] = 3;
    in.Parent[2] = 4;
    in.Parent[3] = 4;
    in.Bone[3] = 0;
    in.Bone[7] = 1;
    in.Bone[0] = 3;
    in.Bone[kPelvis] = 4;
    in.Baseline[0].Translation = {0, 0, 3};
    in.Baseline[1].Translation = {1, 0, 2};
    in.Baseline[2].Translation = {1, 0, 0};
    in.Baseline[3].Translation = {0, 0, 1};
    in.Baseline[2].Scale = 1.25f;
    return in;
}
} // namespace
TEST_CASE("Hierarchical body presentation preserves targets and propagates unposed skin", "[body_pose_presentation]")
{
    auto in = Nested();
    Output out;
    for (const float angle : {0.f, glm::half_pi<float>(), -0.7f})
    {
        in.Rotations[kPelvis] = Yaw(angle);
        REQUIRE(Solve(in, out));
        Identity(out.world[0].Rotation);
        Identity(out.world[1].Rotation);
        Near(out.world[2].Translation, Yaw(angle) * glm::vec3{1, 0, 0});
        CHECK(std::fabs(glm::dot(out.world[2].Rotation, Yaw(angle))) == Approx(1.f).margin(0.0001));
        CHECK(out.world[2].Scale == 1.25f);
    }
    // Suppressed pelvis remains native when HasLegs is off.
    in.HasLegs = false;
    REQUIRE(Solve(in, out));
    Near(out.world[2].Translation, {1, 0, 0});
}
TEST_CASE("Hips shift the entire body once, with tracked crouch and bounded travel", "[body_pose_presentation]")
{
    auto in = Nested();
    Output out;
    in.HasHips = true;
    in.HipOffset = {0, 0, -5};
    REQUIRE(Solve(in, out));
    Near(out.world[4].Translation, {0, 0, -5});
    Near(out.world[0].Translation, {0, 0, -2});
    in.HasLegs = false;
    REQUIRE(Solve(in, out));
    Near(out.world[4].Translation, {0, 0, 0});
    in.HasLegs = true;
    in.HipOffset = {1000, 0, 0};
    REQUIRE(Solve(in, out));
    Near(out.world[4].Translation, {256, 0, 0});
}
TEST_CASE("Invalid hierarchies and values leave output unchanged", "[body_pose_presentation]")
{
    auto in = Nested();
    Output out;
    out.Count = 77;
    out.world[0].Translation = {7, 8, 9};
    const auto reject = [&](const Input& bad)
    {
        CHECK_FALSE(Solve(bad, out));
        CHECK(out.Count == 77);
        Near(out.world[0].Translation, {7, 8, 9});
    };
    auto bad = in;
    bad.Parent[4] = 0;
    reject(bad); // cycle through unsorted nodes
    bad = in;
    bad.Parent[0] = 5;
    reject(bad); // missing parent index
    bad = in;
    bad.Bone[1] = bad.Bone[0];
    reject(bad);
    bad = in;
    bad.Baseline[0].Rotation.w = std::numeric_limits<float>::quiet_NaN();
    reject(bad);
    bad = in;
    bad.Baseline[0].Scale = 0;
    reject(bad);
    bad = in;
    bad.Rotations[3] = {0, 0, 0, 0};
    reject(bad);
    bad = in;
    bad.Count = kMaxNodes + 1;
    reject(bad);
    // Missing wire mappings and independent roots are allowed; no-bones copies baseline unchanged.
    in.Bone[3] = kMissing;
    in.Parent[0] = kMissing;
    in.NoBones = true;
    REQUIRE(Solve(in, out));
    Near(out.world[0].Translation, in.Baseline[0].Translation);
}

// ===================================================================================================== M2d
namespace
{
void NearMargin(const glm::vec3& acActual, const glm::vec3& acExpected, float aMargin)
{
    CHECK(acActual.x == Approx(acExpected.x).margin(aMargin));
    CHECK(acActual.y == Approx(acExpected.y).margin(aMargin));
    CHECK(acActual.z == Approx(acExpected.z).margin(aMargin));
}
bool SameRotation(const glm::quat& acA, const glm::quat& acB, float aMargin = 1e-4f)
{
    return std::fabs(std::fabs(glm::dot(acA, acB)) - 1.f) <= aMargin;
}
// A straight vanilla leg at the copy's native scale 1.03 (skeleton.nif: COM 68.911 above the root, calf 35.595 below the
// thigh, foot 27.95 below the calf, toe 10.661 ahead of the foot), the actor root at (10, 20, 0) yawed by 0.6 rad.
Input Leg(float aNativeScale)
{
    Input in;
    in.Count = 5;
    in.HasData = true;
    in.HasLegs = true;
    in.HasHips = true;
    in.RootRotation = Yaw(0.6f);
    in.RootTranslation = {10.f, 20.f, 0.f};
    const auto at = [&](float x, float y, float z)
    {
        return in.RootTranslation + in.RootRotation * (aNativeScale * glm::vec3{x, y, z});
    };
    in.Parent[1] = 0;
    in.Parent[2] = 1;
    in.Parent[3] = 2;
    in.Parent[4] = 3;
    in.Bone[kPelvis] = 0;
    in.Bone[13] = 1;
    in.Bone[14] = 2;
    in.Bone[15] = 3; // pelvis, left thigh, calf, foot; 4 is the toe
    in.Baseline[0].Translation = at(0.f, 0.f, 68.911f);
    in.Baseline[1].Translation = at(-6.615f, 0.f, 68.911f);
    in.Baseline[2].Translation = at(-6.615f, 0.f, 68.911f - 35.595f);
    in.Baseline[3].Translation = at(-6.615f, 0.f, 68.911f - 35.595f - 27.95f);
    in.Baseline[4].Translation = at(-6.615f, 10.661f, 68.911f - 35.595f - 27.95f);
    for (size_t i = 0; i < in.Count; ++i)
    {
        in.Baseline[i].Rotation = in.RootRotation;
        in.Baseline[i].Scale = aNativeScale;
    }
    for (const size_t wire : {size_t{kPelvis}, size_t{13}, size_t{14}, size_t{15}})
        in.Rotations[wire] = glm::quat{1.f, 0.f, 0.f, 0.f};
    in.HipOffset = {0.f, 0.f, 68.911f}; // the sender (scale 1.000) standing straight
    return in;
}
} // namespace
TEST_CASE("M2d body scale ratio: sender over copy, dead band, clamp, invalid", "[body_pose_presentation]")
{
    const auto d = RootScaleRatio(1.f, 1.03f);
    CHECK(d.Applied);
    CHECK_FALSE(d.DeadBand);
    CHECK_FALSE(d.Clamped);
    CHECK_FALSE(d.Invalid);
    CHECK(d.Ratio == 1.f / 1.03f);
    const auto band = RootScaleRatio(1.f, 1.002f);
    CHECK(band.DeadBand);
    CHECK_FALSE(band.Applied);
    CHECK(band.Ratio == 1.f);
    CHECK(RootScaleRatio(1.0029f, 1.f).DeadBand); // inside the band (upstream: change only above 0.003)
    CHECK(RootScaleRatio(.9971f, 1.f).DeadBand);
    CHECK(RootScaleRatio(1.0035f, 1.f).Applied);
    const auto big = RootScaleRatio(2.f, 1.f), small = RootScaleRatio(.3f, 1.f);
    CHECK(big.Clamped);
    CHECK(big.Applied);
    CHECK(big.Ratio == kScaleMax);
    CHECK(small.Clamped);
    CHECK(small.Ratio == kScaleMin);
    for (const auto& bad :
         {RootScaleRatio(0.f, 1.f), RootScaleRatio(1.f, 0.f), RootScaleRatio(-1.f, 1.f), RootScaleRatio(std::numeric_limits<float>::quiet_NaN(), 1.f),
          RootScaleRatio(1.f, std::numeric_limits<float>::infinity())})
    {
        CHECK(bad.Invalid);
        CHECK_FALSE(bad.Applied);
        CHECK(bad.Ratio == 1.f);
    }
}
TEST_CASE("M2d body scale as world values puts the feet where the sender's are", "[body_pose_presentation]")
{
    // As built (no scale): the 1.03 legs hang from the sender's 68.911 hip, the foot sinks by 0.03 x the leg length.
    auto in = Leg(1.03f);
    Output out;
    REQUIRE(Solve(in, out));
    const auto rootSpace = [&](size_t i)
    {
        return RootSpace(in.RootRotation, in.RootTranslation, out.world[i].Translation);
    };
    NearMargin(rootSpace(0), {0.f, 0.f, 68.911f}, 1e-3f);
    CHECK(rootSpace(3).z == Approx(68.911f - 1.03f * (35.595f + 27.95f)).margin(1e-3));
    CHECK(rootSpace(3).z < 68.911f - 35.595f - 27.95f - 1.8f); // about 1.9 units low
    // Scaled about the skeleton root (here at the actor root) by sender / native: the sender's own foot height.
    std::array<bool, kMaxNodes> under{};
    for (size_t i = 0; i < in.Count; ++i)
        under[i] = true;
    const auto d = RootScaleRatio(1.f, 1.03f);
    in = Leg(1.03f);
    REQUIRE(ApplyRootScale(in, under, in.RootTranslation, d.Ratio));
    for (size_t i = 0; i < in.Count; ++i)
        CHECK(in.Baseline[i].Scale == 1.03f * d.Ratio); // one float multiply, reproducible bits
    REQUIRE(Solve(in, out));
    NearMargin(rootSpace(0), {0.f, 0.f, 68.911f}, 1e-3f); // pelvis at the sent hip offset (true stance, D3)
    NearMargin(rootSpace(3), {-6.615f, 0.f, 68.911f - 35.595f - 27.95f}, 2e-3f);
    NearMargin(rootSpace(4), {-6.615f, 10.661f, 68.911f - 35.595f - 27.95f}, 2e-3f);
    for (size_t i = 0; i < in.Count; ++i)
        CHECK(out.world[i].Scale == in.Baseline[i].Scale);
    // Unmarked entries keep their values; the pivot is respected; bad inputs leave the input untouched.
    auto partial = Leg(1.03f);
    const auto before = partial;
    under.fill(false);
    under[3] = true;
    const glm::vec3 pivot{1.f, 2.f, 3.f};
    REQUIRE(ApplyRootScale(partial, under, pivot, .5f));
    NearMargin(partial.Baseline[3].Translation, pivot + .5f * (before.Baseline[3].Translation - pivot), 1e-4f);
    CHECK(partial.Baseline[3].Scale == 1.03f * .5f);
    CHECK(partial.Baseline[2].Translation == before.Baseline[2].Translation);
    CHECK(partial.Baseline[2].Scale == before.Baseline[2].Scale);
    auto untouched = Leg(1.03f);
    CHECK_FALSE(ApplyRootScale(untouched, under, glm::vec3{std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f}, .9f));
    CHECK_FALSE(ApplyRootScale(untouched, under, pivot, 2.f));
    CHECK(untouched.Baseline[3].Translation == before.Baseline[3].Translation);
}
namespace
{
// A bent right arm: upper arm 0 (shoulder), forearm 1 (elbow), hand 2 (wrist), a finger 3 below the hand, and a twist
// bone 4 below the upper arm; root yawed.
Output Arm()
{
    Output out;
    out.Count = 5;
    const glm::quat root = Yaw(0.4f);
    out.world[0].Translation = root * glm::vec3{0.f, 0.f, 100.f};
    out.world[1].Translation = root * glm::vec3{20.f, 0.f, 90.f};
    out.world[2].Translation = root * glm::vec3{20.f, 15.f, 85.f};
    out.world[3].Translation = root * glm::vec3{21.f, 19.f, 85.f};
    out.world[4].Translation = root * glm::vec3{8.f, 0.f, 96.f};
    for (size_t i = 0; i < out.Count; ++i)
        out.world[i].Rotation = root * glm::angleAxis(.3f * static_cast<float>(i), glm::normalize(glm::vec3{1.f, 2.f, 3.f}));
    return out;
}
std::array<int, kMaxNodes> ArmParents()
{
    std::array<int, kMaxNodes> parent{};
    parent.fill(kMissing);
    parent[1] = 0;
    parent[2] = 1;
    parent[3] = 2;
    parent[4] = 0;
    return parent;
}
} // namespace
TEST_CASE("M2d hand fine-tuning is upstream ReachHands: two-bone IK, elbow side kept, hand orientation restored", "[body_pose_presentation]")
{
    const glm::quat root = Yaw(0.4f);
    const auto parent = ArmParents();
    auto out = Arm();
    const auto before = out;
    const glm::vec3 target = out.world[2].Translation + root * glm::vec3{3.f, -2.f, 4.f};
    const glm::quat handWanted = root * Yaw(0.5f);
    const float upper = glm::distance(before.world[0].Translation, before.world[1].Translation);
    const float fore = glm::distance(before.world[1].Translation, before.world[2].Translation);
    const auto r = ReachHand(out, parent, root, 0, 1, 2, target, handWanted);
    REQUIRE(r.Outcome == Reach::Applied);
    CHECK(r.GapBefore == Approx(std::sqrt(29.f)).margin(1e-4));
    NearMargin(r.ErrorBefore, {-3.f, 2.f, -4.f}, 1e-4f); // copy hand minus target, root space: the measure BEFORE IK
    CHECK(r.GapAfter < 1e-3f);
    NearMargin(out.world[2].Translation, target, 1e-3f);
    NearMargin(out.world[0].Translation, before.world[0].Translation, 1e-6f); // the shoulder joint does not move
    CHECK(glm::distance(out.world[0].Translation, out.world[1].Translation) == Approx(upper).margin(1e-3));
    CHECK(glm::distance(out.world[1].Translation, out.world[2].Translation) == Approx(fore).margin(1e-3));
    CHECK(SameRotation(out.world[2].Rotation, handWanted));
    // The elbow bends to the same side; the finger keeps its place in the hand's frame; the twist bone follows the upper arm.
    const glm::vec3 bendBefore = glm::cross(before.world[1].Translation - before.world[0].Translation, before.world[2].Translation - before.world[0].Translation);
    const glm::vec3 bendAfter = glm::cross(out.world[1].Translation - out.world[0].Translation, out.world[2].Translation - out.world[0].Translation);
    CHECK(glm::dot(bendBefore, bendAfter) > 0.f);
    const glm::vec3 fingerLocal = glm::inverse(before.world[2].Rotation) * (before.world[3].Translation - before.world[2].Translation);
    NearMargin(out.world[3].Translation, out.world[2].Translation + handWanted * fingerLocal, 1e-3f);
    CHECK(SameRotation(out.world[3].Rotation, handWanted * glm::inverse(before.world[2].Rotation) * before.world[3].Rotation));
    CHECK(glm::distance(out.world[4].Translation, out.world[0].Translation) == Approx(glm::distance(before.world[4].Translation, before.world[0].Translation)).margin(1e-3));
    for (size_t i = 0; i < out.Count; ++i)
        CHECK(Unit(out.world[i].Rotation));
}
TEST_CASE("M2d hand fine-tuning refuses large gaps and bad reads, and stops at full reach", "[body_pose_presentation]")
{
    const glm::quat root = Yaw(0.4f);
    const auto parent = ArmParents();
    const auto same = [](const Output& a, const Output& b)
    {
        for (size_t i = 0; i < a.Count; ++i)
            if (!(a.world[i].Translation == b.world[i].Translation) || !(a.world[i].Rotation == b.world[i].Rotation))
                return false;
        return true;
    };
    auto out = Arm();
    const auto before = out;
    // More than 15 units from the hand: the arm keeps the sender's pose untouched (upstream cMaxCorrection).
    auto r = ReachHand(out, parent, root, 0, 1, 2, out.world[2].Translation + glm::vec3{0.f, 0.f, 15.5f}, root);
    CHECK(r.Outcome == Reach::GapCap);
    CHECK(r.GapBefore == Approx(15.5f).margin(1e-3));
    CHECK(r.GapAfter == Approx(15.5f).margin(1e-3));
    CHECK(same(out, before));
    // A hierarchy where the hand does not hang below the forearm, a bad index, a non-finite target: bad read, untouched.
    auto broken = parent;
    broken[2] = 0;
    CHECK(ReachHand(out, broken, root, 0, 1, 2, before.world[2].Translation, root).Outcome == Reach::BadRead);
    CHECK(ReachHand(out, parent, root, 0, 1, 7, before.world[2].Translation, root).Outcome == Reach::BadRead);
    CHECK(ReachHand(out, parent, root, 0, 1, 2, glm::vec3{std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f}, root).Outcome == Reach::BadRead);
    CHECK(same(out, before));
    // A target just beyond full reach (within the cap): the hand lands on the line to it, half a unit short of straight.
    const float upper = glm::distance(before.world[0].Translation, before.world[1].Translation);
    const float fore = glm::distance(before.world[1].Translation, before.world[2].Translation);
    const glm::vec3 shoulder = before.world[0].Translation;
    const glm::vec3 direction = glm::normalize(before.world[2].Translation - shoulder + root * glm::vec3{4.f, 4.f, 0.f});
    const glm::vec3 far = shoulder + (upper + fore + 1.f) * direction;
    REQUIRE(glm::distance(far, before.world[2].Translation) <= kReachMaxCorrection);
    r = ReachHand(out, parent, root, 0, 1, 2, far, root);
    CHECK(r.Outcome == Reach::Applied);
    NearMargin(out.world[2].Translation, shoulder + (upper + fore - .5f) * direction, 2e-3f);
    CHECK(r.GapAfter == Approx(1.5f).margin(2e-3));
}
TEST_CASE("M2d rotation helpers and descendant test", "[body_pose_presentation]")
{
    const glm::vec3 x{1.f, 0.f, 0.f}, y{0.f, 1.f, 0.f};
    NearMargin(RotationBetween(x, y) * x, y, 1e-6f);
    CHECK(SameRotation(RotationBetween(x, x), glm::quat{1.f, 0.f, 0.f, 0.f}));
    CHECK(SameRotation(RotationBetween(x, -x), glm::quat{1.f, 0.f, 0.f, 0.f})); // exactly opposite: no turn (upstream)
    const auto parent = ArmParents();
    CHECK(Below(parent, 5, 3, 0));
    CHECK(Below(parent, 5, 3, 2));
    CHECK(Below(parent, 5, 4, 0));
    CHECK_FALSE(Below(parent, 5, 4, 1));
    CHECK_FALSE(Below(parent, 5, 0, 0));
    CHECK_FALSE(Below(parent, 5, 9, 0));
    auto cyclic = parent;
    cyclic[0] = 3;
    CHECK_FALSE(Below(cyclic, 5, 4, 7)); // a cycle ends, it does not hang
    Output out;
    out.Count = 2;
    out.world[1].Translation = {1.f, 0.f, 0.f};
    CHECK(ValidOutput(out));
    out.world[1].Scale = 0.f;
    CHECK_FALSE(ValidOutput(out));
    out.world[1].Scale = 1.f;
    out.world[1].Translation.y = std::numeric_limits<float>::infinity();
    CHECK_FALSE(ValidOutput(out));
}
TEST_CASE("M2d fingers: world chains below the posed hand from parent-relative rotations and own locals", "[body_pose_presentation]")
{
    Output out;
    out.Count = 20;
    const int hand = 2;
    out.world[hand].Translation = {5.f, 6.f, 7.f};
    out.world[hand].Rotation = Yaw(1.2f);
    out.world[hand].Scale = .97f;
    std::array<int, kFingerBonesPerHand> entries{};
    std::array<glm::vec3, kFingerBonesPerHand> local{};
    std::array<glm::quat, kFingerBonesPerHand> rotation{};
    for (size_t k = 0; k < kFingerBonesPerHand; ++k)
    {
        entries[k] = static_cast<int>(3 + k);
        local[k] = {0.5f * static_cast<float>(k % 3), 1.f, 3.f - static_cast<float>(k) * .1f};
        rotation[k] = glm::angleAxis(.1f * static_cast<float>(k + 1), glm::normalize(glm::vec3{1.f, 0.f, 1.f}));
        out.world[static_cast<size_t>(entries[k])].Scale = .97f;
        out.world[static_cast<size_t>(entries[k])].Translation = {99.f, 99.f, 99.f}; // carried by Solve; replaced here
    }
    const auto before = out;
    REQUIRE(ApplyFingers(out, hand, entries, local, rotation));
    for (size_t f = 0; f < kFingerChains; ++f)
    {
        Transform parent = before.world[hand];
        for (size_t k = 0; k < kFingerChainBones; ++k)
        {
            const size_t at = f * kFingerChainBones + k;
            const auto& world = out.world[static_cast<size_t>(entries[at])];
            const glm::quat rotationWanted = parent.Rotation * rotation[at];
            const glm::vec3 positionWanted = parent.Translation + parent.Rotation * (local[at] * parent.Scale);
            CHECK(SameRotation(world.Rotation, rotationWanted, 1e-5f));
            NearMargin(world.Translation, positionWanted, 1e-4f);
            CHECK(world.Scale == .97f); // own world scale kept
            parent = world;
        }
    }
    CHECK(out.world[hand].Translation == before.world[hand].Translation); // the hand itself is not written
    auto bad = before;
    auto badEntries = entries;
    badEntries[4] = 40;
    CHECK_FALSE(ApplyFingers(bad, hand, badEntries, local, rotation));
    auto badRotation = rotation;
    badRotation[7] = {0.f, 0.f, 0.f, 0.f};
    CHECK_FALSE(ApplyFingers(bad, hand, entries, local, badRotation));
    auto badLocal = local;
    badLocal[0].x = std::numeric_limits<float>::infinity();
    CHECK_FALSE(ApplyFingers(bad, hand, entries, badLocal, rotation));
    CHECK_FALSE(ApplyFingers(bad, 30, entries, local, rotation));
    for (size_t k = 0; k < kFingerBonesPerHand; ++k)
        CHECK(bad.world[static_cast<size_t>(entries[k])].Translation == glm::vec3{99.f, 99.f, 99.f});
}
