#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Stl.hpp>

#include <catch2/catch.hpp>

#include <CorpseRestPose.h>

#include <glm/gtc/quaternion.hpp>

using namespace CorpseRestPose;

namespace
{
VRPose Lying(const float aX, const float aTurnDegrees = 0.f)
{
    VRPose pose{};
    pose.HasData = true;
    pose.HasLegs = true;
    pose.HasRootPosition = true;
    pose.RootPosition[0] = aX;
    pose.RootPosition[1] = 20.f;
    pose.RootPosition[2] = -5.f;
    for (auto& bone : pose.Bones)
        bone = glm::angleAxis(glm::radians(30.f), glm::vec3{0.f, 0.f, 1.f});
    pose.Bones[VRPose::kLeftCalf] = glm::angleAxis(glm::radians(30.f + aTurnDegrees), glm::vec3{0.f, 0.f, 1.f});
    return pose;
}

RagdollPose Wolf(const float aX, const int16_t aPawShift = 0)
{
    RagdollPose pose{};
    pose.Layout = 0xABCD;
    pose.BodyCount = 3;
    pose.Root[0] = aX;
    pose.Bodies.resize(3);
    for (auto& body : pose.Bodies)
        body.Rotation = glm::quat{1.f, 0.f, 0.f, 0.f};
    pose.Bodies[2].Offset = {static_cast<int16_t>(40 + aPawShift), 8, 0};
    return pose;
}

const auto cSameRest = [](const VRPose& acBefore, const VRPose& acNow)
{
    return SameRest(acBefore, acNow);
};
const auto cSameBodies = [](const RagdollPose& acBefore, const RagdollPose& acNow)
{
    return SameRest(acBefore, acNow);
};
} // namespace

TEST_CASE("a dead body's pose is taken from its owner, an unposed report changes nothing, a living actor's is forgotten", "[corpse_rest_pose]")
{
    CHECK(OnReport(true, true, true) == Report::kTake);
    // No pose, or one outside the world: what is kept stays.
    CHECK(OnReport(true, false, true) == Report::kIgnore);
    CHECK(OnReport(true, true, false) == Report::kIgnore);
    CHECK(OnReport(true, false, false) == Report::kIgnore);
    // Revived, or never dead.
    CHECK(OnReport(false, true, true) == Report::kForget);
    CHECK(OnReport(false, false, false) == Report::kForget);
}

TEST_CASE("a person's pose repeated is at rest; a turned bone, a moved root or another shape is not", "[corpse_rest_pose]")
{
    CHECK(SameRest(Lying(0.f), Lying(0.f)));
    // A settled ragdoll still twitches.
    CHECK(SameRest(Lying(0.f), Lying(3.f, 1.f)));
    CHECK_FALSE(SameRest(Lying(0.f), Lying(5.f)));
    CHECK_FALSE(SameRest(Lying(0.f), Lying(0.f, 10.f)));

    VRPose upper = Lying(0.f);
    upper.HasLegs = false;
    CHECK_FALSE(SameRest(Lying(0.f), upper));
    // Only the upper body is looked at without legs.
    VRPose upperTurnedLeg = Lying(0.f, 40.f);
    upperTurnedLeg.HasLegs = false;
    CHECK(SameRest(upper, upperTurnedLeg));

    VRPose boneless = Lying(0.f);
    boneless.NoBones = true;
    CHECK_FALSE(SameRest(boneless, boneless));
    CHECK_FALSE(SameRest(VRPose{}, VRPose{}));
}

TEST_CASE("a creature's bodies repeated are at rest; a moved body, root or another skeleton is not", "[corpse_rest_pose]")
{
    CHECK(SameRest(Wolf(0.f), Wolf(0.f)));
    CHECK(SameRest(Wolf(0.f), Wolf(1.f, 8)));
    CHECK_FALSE(SameRest(Wolf(0.f), Wolf(10.f)));
    CHECK_FALSE(SameRest(Wolf(0.f), Wolf(0.f, 40)));

    RagdollPose turned = Wolf(0.f);
    turned.Bodies[1].Rotation = glm::angleAxis(glm::radians(20.f), glm::vec3{1.f, 0.f, 0.f});
    CHECK_FALSE(SameRest(Wolf(0.f), turned));

    RagdollPose other = Wolf(0.f);
    other.Layout = 0x1234;
    CHECK_FALSE(SameRest(Wolf(0.f), other));
    CHECK_FALSE(SameRest(RagdollPose{}, RagdollPose{}));
}

TEST_CASE("a body's pose is kept once it repeats, dropped while it moves, and forgotten on revive or removal", "[corpse_rest_pose]")
{
    Store<VRPose> store;
    // The first pose says nothing of rest yet.
    store.Take(7, Lying(0.f), cSameRest);
    CHECK(store.Resting(7) == nullptr);
    store.Take(7, Lying(0.f), cSameRest);
    REQUIRE(store.Resting(7) != nullptr);
    CHECK(store.Resting(7)->RootPosition[0] == 0.f);

    // Moved again: nothing kept until it rests again, then the new rest.
    store.Take(7, Lying(100.f), cSameRest);
    CHECK(store.Resting(7) == nullptr);
    store.Take(7, Lying(101.f), cSameRest);
    REQUIRE(store.Resting(7) != nullptr);
    CHECK(store.Resting(7)->RootPosition[0] == 101.f);

    store.Forget(7);
    CHECK(store.Resting(7) == nullptr);
    CHECK(store.Size() == 0);
    // Forgetting what is not kept is nothing.
    store.Forget(8);
    CHECK(store.Size() == 0);
}

TEST_CASE("a creature's bodies are kept the same way", "[corpse_rest_pose]")
{
    Store<RagdollPose> store;
    store.Take(3, Wolf(0.f), cSameBodies);
    store.Take(3, Wolf(0.f), cSameBodies);
    REQUIRE(store.Resting(3) != nullptr);
    CHECK(store.Resting(3)->Bodies.size() == 3);
    store.Take(3, Wolf(0.f, 64), cSameBodies);
    CHECK(store.Resting(3) == nullptr);
}

TEST_CASE("past the cap the body taken longest ago goes first", "[corpse_rest_pose]")
{
    Store<VRPose, 3> store;
    for (uint32_t id = 1; id <= 3; ++id)
    {
        store.Take(id, Lying(0.f), cSameRest);
        store.Take(id, Lying(0.f), cSameRest);
    }
    // Body 1 taken again: body 2 is now the oldest.
    store.Take(1, Lying(0.f), cSameRest);
    store.Take(4, Lying(0.f), cSameRest);
    CHECK(store.Size() == 3);
    CHECK(store.Resting(1) != nullptr);
    CHECK(store.Resting(2) == nullptr);
    CHECK(store.Resting(3) != nullptr);
    // Body 4 has only its first pose.
    CHECK(store.Resting(4) == nullptr);
    store.Take(4, Lying(0.f), cSameRest);
    CHECK(store.Resting(4) != nullptr);

    // A forgotten body frees its place.
    store.Forget(3);
    store.Take(5, Lying(0.f), cSameRest);
    CHECK(store.Size() == 3);
    CHECK(store.Resting(1) != nullptr);
}
