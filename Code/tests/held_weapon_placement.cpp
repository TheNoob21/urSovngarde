#include <catch2/catch.hpp>

#include <Games/Skyrim/HeldWeaponPlacement.h>

#include <glm/gtc/matrix_transform.hpp>

namespace
{
constexpr float kNock = 64.f;

bool Near(const glm::vec3& acA, const glm::vec3& acB, const float aWithin = 0.01f)
{
    return glm::distance(acA, acB) <= aWithin;
}

bool IsRotation(const glm::mat3& acM)
{
    const glm::mat3 cIdentity = glm::transpose(acM) * acM;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (std::abs(cIdentity[i][j] - (i == j ? 1.f : 0.f)) > 1e-4f)
                return false;
    return std::abs(glm::determinant(acM) - 1.f) < 1e-4f;
}
} // namespace

TEST_CASE("an arrow on the string points from the string hand to the bow hand, its nock on the string", "[arrow]")
{
    const glm::vec3 cString{10.f, -20.f, 100.f};
    for (const glm::vec3 cBow : {glm::vec3{10.f, 40.f, 100.f}, glm::vec3{60.f, 10.f, 130.f}, glm::vec3{-30.f, -60.f, 80.f}})
    {
        HeldWeaponPlacement::Placement placed;
        REQUIRE(HeldWeaponPlacement::OnString(cString, cBow, kNock, placed));
        const glm::vec3 cForward = glm::normalize(cBow - cString);
        CHECK(Near(placed.Rotation[1], cForward, 1e-4f));
        CHECK(IsRotation(placed.Rotation));
        // The origin is the head; the nock is one arrow length back along the arrow's y axis.
        CHECK(Near(placed.Position - placed.Rotation[1] * kNock, cString, 0.1f));
    }
}

TEST_CASE("an arrow drawn straight up or down still gets a whole rotation", "[arrow]")
{
    for (const float cDz : {80.f, -80.f})
    {
        HeldWeaponPlacement::Placement placed;
        REQUIRE(HeldWeaponPlacement::OnString({0.f, 0.f, 0.f}, {0.f, 0.f, cDz}, kNock, placed));
        CHECK(IsRotation(placed.Rotation));
        CHECK(Near(placed.Rotation[1], {0.f, 0.f, cDz > 0.f ? 1.f : -1.f}, 1e-4f));
    }
}

TEST_CASE("hands on top of each other leave the arrow where it is", "[arrow]")
{
    HeldWeaponPlacement::Placement placed;
    placed.Position = {1.f, 2.f, 3.f};
    CHECK_FALSE(HeldWeaponPlacement::OnString({5.f, 5.f, 5.f}, {5.5f, 5.f, 5.f}, kNock, placed));
    CHECK(Near(placed.Position, {1.f, 2.f, 3.f}));
}

TEST_CASE("a held weapon or arrow follows the hand's turn and scale", "[arrow]")
{
    const glm::vec3 cOffset{1.f, 2.f, 3.f};
    const glm::quat cGrip = glm::angleAxis(glm::radians(30.f), glm::vec3{1.f, 0.f, 0.f});

    // A hand that is not turned: the grip and offset stay as sent, the offset scaled with the hand.
    auto placed = HeldWeaponPlacement::InHand(glm::mat3{1.f}, {100.f, 0.f, 50.f}, 2.f, cGrip, cOffset);
    CHECK(Near(placed.Position, {102.f, 4.f, 56.f}));
    CHECK(Near(placed.Rotation[1], glm::mat3_cast(cGrip)[1], 1e-4f));

    // A hand turned a quarter about z carries the offset round with it.
    const glm::mat3 cTurned = glm::mat3(glm::rotate(glm::mat4{1.f}, glm::radians(90.f), glm::vec3{0.f, 0.f, 1.f}));
    placed = HeldWeaponPlacement::InHand(cTurned, {0.f, 0.f, 0.f}, 1.f, glm::quat{1.f, 0.f, 0.f, 0.f}, cOffset);
    CHECK(Near(placed.Position, {-2.f, 1.f, 3.f}, 1e-4f));
    CHECK(IsRotation(placed.Rotation));
}

TEST_CASE("what hangs below a placed weapon follows it from its own local", "[arrow]")
{
    // The parent turned a quarter about z, at (10, 0, 0), scale 2; the child one unit along x and turned the same way.
    const glm::mat3 cQuarter = glm::mat3(glm::rotate(glm::mat4{1.f}, glm::radians(90.f), glm::vec3{0.f, 0.f, 1.f}));
    const HeldWeaponPlacement::Frame cParent{cQuarter, {10.f, 0.f, 0.f}, 2.f};
    const HeldWeaponPlacement::Frame cLocal{cQuarter, {1.f, 0.f, 0.f}, 1.5f};
    const auto cWorld = HeldWeaponPlacement::Compose(cParent, cLocal);
    CHECK(Near(cWorld.Position, {10.f, 2.f, 0.f}, 1e-4f));
    CHECK(std::abs(cWorld.Scale - 3.f) < 1e-5f);
    CHECK(IsRotation(cWorld.Rotation));
    // Two quarter turns: x points along -x.
    CHECK(Near(cWorld.Rotation[0], {-1.f, 0.f, 0.f}, 1e-4f));
    // An identity local leaves the parent's world as it is.
    const auto cSame = HeldWeaponPlacement::Compose(cParent, {});
    CHECK(Near(cSame.Position, cParent.Position));
    CHECK(Near(cSame.Rotation[1], cParent.Rotation[1], 1e-5f));
}

TEST_CASE("a nocked arrow with the hands close points where the owner faces, not along the hands", "[arrow]")
{
    // The string hand beside the bow hand, 18 units to its left: the line between them points left, the body faces +y.
    const glm::vec3 cString{0.f, 0.f, 100.f};
    for (const glm::vec3 cBow : {glm::vec3{-18.f, 0.f, 100.f}, glm::vec3{0.f, -18.f, 100.f}, glm::vec3{-10.f, 8.f, 104.f}})
    {
        HeldWeaponPlacement::Placement placed;
        REQUIRE(HeldWeaponPlacement::OnStringAimed(cString, cBow, {0.f, 1.f, 0.f}, kNock, placed));
        CHECK(Near(placed.Rotation[1], {0.f, 1.f, 0.f}, 1e-4f));
        CHECK(IsRotation(placed.Rotation));
        CHECK(Near(placed.Position - placed.Rotation[1] * kNock, cString, 0.1f));
    }
}

TEST_CASE("a drawn arrow follows the hands, and blends in between", "[arrow]")
{
    const glm::vec3 cString{0.f, 0.f, 100.f};
    const glm::vec3 cAim{0.f, 1.f, 0.f};
    HeldWeaponPlacement::Placement placed;
    // Well apart: along the hands, whatever the body faces.
    REQUIRE(HeldWeaponPlacement::OnStringAimed(cString, {-60.f, 0.f, 100.f}, cAim, kNock, placed));
    CHECK(Near(placed.Rotation[1], {-1.f, 0.f, 0.f}, 1e-4f));
    // In between: turned part of the way from the facing to the hands, and always a whole rotation.
    REQUIRE(HeldWeaponPlacement::OnStringAimed(cString, {-HeldWeaponPlacement::cNockReach - 7.5f, 0.f, 100.f}, cAim, kNock, placed));
    CHECK(placed.Rotation[1].x < -0.1f);
    CHECK(placed.Rotation[1].y > 0.1f);
    CHECK(IsRotation(placed.Rotation));
    CHECK(glm::length(placed.Rotation[1]) == Approx(1.f).margin(1e-4f));
}

TEST_CASE("a close nock follows the bow when the player aims up and sideways", "[arrow]")
{
    const glm::vec3 cForward = glm::normalize(glm::vec3{1.f, 0.f, 1.f});
    const glm::vec3 cLimb = glm::normalize(glm::vec3{-1.f, 0.f, 1.f}) * 50.f;
    const glm::vec3 cGrip = cForward * 15.f;
    const auto cBowDirection = HeldWeaponPlacement::BowDirection(cGrip, -cLimb, cLimb);
    REQUIRE(cBowDirection);
    CHECK(Near(*cBowDirection, cForward, 1e-4f));
    for (const float side : {-1.f, 1.f})
    {
        HeldWeaponPlacement::Placement placed;
        REQUIRE(HeldWeaponPlacement::OnStringAimed({}, {0.f, side * 15.f, 0.f}, *cBowDirection, kNock, placed));
        CHECK(Near(placed.Rotation[1], cForward, 1e-4f));
    }
    CHECK_FALSE(HeldWeaponPlacement::BowDirection(cGrip, {}, {}));
}

TEST_CASE("a string hand ahead of the bow hand is not taken for a draw backwards", "[arrow]")
{
    // The arrow hand reached 30 units past the bow hand along the facing: the line points back at the body.
    HeldWeaponPlacement::Placement placed;
    REQUIRE(HeldWeaponPlacement::OnStringAimed({0.f, 30.f, 100.f}, {0.f, 0.f, 100.f}, {0.f, 1.f, 0.f}, kNock, placed));
    CHECK(Near(placed.Rotation[1], {0.f, 1.f, 0.f}, 1e-4f));
    // A real draw far enough apart still follows the hands, even backwards.
    REQUIRE(HeldWeaponPlacement::OnStringAimed({0.f, 70.f, 100.f}, {0.f, 0.f, 100.f}, {0.f, 1.f, 0.f}, kNock, placed));
    CHECK(Near(placed.Rotation[1], {0.f, -1.f, 0.f}, 1e-4f));
}

TEST_CASE("hands on top of each other still lay the arrow along the facing", "[arrow]")
{
    HeldWeaponPlacement::Placement placed;
    REQUIRE(HeldWeaponPlacement::OnStringAimed({5.f, 5.f, 5.f}, {5.5f, 5.f, 5.f}, {1.f, 0.f, 0.f}, kNock, placed));
    CHECK(Near(placed.Rotation[1], {1.f, 0.f, 0.f}, 1e-4f));
    // No facing known: as OnString, which leaves the arrow be when the hands tell nothing.
    CHECK_FALSE(HeldWeaponPlacement::OnStringAimed({5.f, 5.f, 5.f}, {5.5f, 5.f, 5.f}, {0.f, 0.f, 0.f}, kNock, placed));
    REQUIRE(HeldWeaponPlacement::OnStringAimed({0.f, 0.f, 0.f}, {0.f, 40.f, 0.f}, {0.f, 0.f, 0.f}, kNock, placed));
    CHECK(Near(placed.Rotation[1], {0.f, 1.f, 0.f}, 1e-4f));
}

TEST_CASE("only hands in a draw pull the string", "[arrow]")
{
    CHECK_FALSE(HeldWeaponPlacement::IsDraw(18.f));
    CHECK_FALSE(HeldWeaponPlacement::IsDraw(HeldWeaponPlacement::cNockReach - 0.1f));
    CHECK(HeldWeaponPlacement::IsDraw(HeldWeaponPlacement::cNockReach));
    CHECK(HeldWeaponPlacement::IsDraw(70.f));
}

TEST_CASE("the bow is in the hand the arrow was not taken in", "[arrow]")
{
    // The arrow in the right hand: the bow is in the left, whatever the grip test said.
    CHECK(HeldWeaponPlacement::BowSide(1, true, false, true) == 0);
    CHECK(HeldWeaponPlacement::BowSide(1, false, false, false) == 0);
    CHECK(HeldWeaponPlacement::BowSide(0, true, false, false) == 1);
    CHECK(HeldWeaponPlacement::BowSide(0, true, true, false) == 1);
}

TEST_CASE("the bow side falls back to where a weapon is held, then to the left", "[arrow]")
{
    CHECK(HeldWeaponPlacement::BowSide(-1, true, false, true) == 1);
    CHECK(HeldWeaponPlacement::BowSide(-1, true, true, false) == 0);
    CHECK(HeldWeaponPlacement::BowSide(-1, true, true, true) == 0);
    CHECK(HeldWeaponPlacement::BowSide(-1, true, false, false) == 0);
    CHECK(HeldWeaponPlacement::BowSide(-1, false, false, true) == 0);
}
