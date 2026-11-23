#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>

#include <catch2/catch.hpp>

#include <cmath>
#include <limits>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Services/HeldObjectPlayback.h>

using namespace HeldObject;

namespace
{
Pose At(const glm::vec3& acPosition, const float aYaw = 0.f)
{
    return {acPosition, glm::angleAxis(aYaw, glm::vec3(0.f, 0.f, 1.f))};
}

void NearPosition(const glm::vec3& acA, const glm::vec3& acB, const float aMargin)
{
    INFO("(" << acA.x << ", " << acA.y << ", " << acA.z << ") vs (" << acB.x << ", " << acB.y << ", " << acB.z << ")");
    CHECK(glm::distance(acA, acB) <= aMargin);
}
} // namespace

TEST_CASE("a reference's angles and its 3D's turn convert both ways", "[heldobjects]")
{
    for (const glm::vec3 angles : {glm::vec3(0.f), glm::vec3(0.3f, -0.4f, 1.2f), glm::vec3(-1.f, 0.7f, -2.9f), glm::vec3(2.f, 1.2f, 0.1f)})
    {
        const glm::vec3 back = ToAngles(FromAngles(angles));
        INFO(angles.x << " " << angles.y << " " << angles.z);
        // Same turn either way (angle sets can differ by whole turns).
        CHECK(AngleBetween(FromAngles(back), FromAngles(angles)) < 3e-3f);
        if (std::abs(angles.y) < 1.5f)
        {
            CHECK(back.x == Approx(angles.x).margin(1e-4));
            CHECK(back.y == Approx(angles.y).margin(1e-4));
            CHECK(back.z == Approx(angles.z).margin(1e-4));
        }
    }
}

TEST_CASE("an object is shown between the updates around the moment one delay ago", "[heldobjects]")
{
    Playback playback;
    REQUIRE(playback.IsEmpty());
    playback.Add(1000.0, At({0.f, 0.f, 0.f}, 0.f), false);
    playback.Add(1040.0, At({4.f, 0.f, 0.f}, 0.4f), false);
    playback.Add(1080.0, At({8.f, 0.f, 0.f}, 0.8f), false);

    // 40 ms between updates: shown 50 ms behind.
    CHECK(playback.GetDelay() == Approx(50.0));

    // Halfway between the first two.
    const Pose cHalf = playback.At(1070.0, 50.0);
    NearPosition(cHalf.Position, {2.f, 0.f, 0.f}, 1e-4f);
    CHECK(AngleBetween(cHalf.Rotation, glm::angleAxis(0.2f, glm::vec3(0.f, 0.f, 1.f))) < 3e-3f);

    // Before the first and after the newest: the ends, never further.
    NearPosition(playback.At(900.0, 50.0).Position, {0.f, 0.f, 0.f}, 1e-6f);
    NearPosition(playback.At(2000.0, 50.0).Position, {8.f, 0.f, 0.f}, 1e-6f);
}

TEST_CASE("the delay follows the gaps between updates, within bounds", "[heldobjects]")
{
    Playback playback;
    playback.Add(0.0, At({0.f, 0.f, 0.f}), false);
    CHECK(playback.GetDelay() == Approx(Playback::kMinDelay));
    playback.Add(10.0, At({1.f, 0.f, 0.f}), false);
    CHECK(playback.GetDelay() == Approx(Playback::kMinDelay));
    playback.Add(110.0, At({2.f, 0.f, 0.f}), false);
    CHECK(playback.GetDelay() == Approx(110.0));
    playback.Add(400.0, At({3.f, 0.f, 0.f}), false);
    CHECK(playback.GetDelay() == Approx(Playback::kMaxDelay));
}

TEST_CASE("after a pause the object goes on from where it stopped", "[heldobjects]")
{
    Playback playback;
    playback.Add(0.0, At({0.f, 0.f, 0.f}), false);
    playback.Add(40.0, At({4.f, 0.f, 0.f}), false);
    // A hand held still sends nothing for a while, then moves on.
    playback.Add(2000.0, At({10.f, 0.f, 0.f}), false);
    REQUIRE(playback.GetCount() == 2);
    // Not a jump to the new place: it starts from the last one.
    NearPosition(playback.At(2000.0, Playback::kMinDelay).Position, {4.f, 0.f, 0.f}, 1e-4f);
    NearPosition(playback.At(2000.0 + Playback::kMinDelay / 2.0, Playback::kMinDelay).Position, {7.f, 0.f, 0.f}, 1e-3f);
}

TEST_CASE("updates arriving out of order start the playback again, and only the latest few are kept", "[heldobjects]")
{
    Playback playback;
    for (int i = 0; i < 20; ++i)
        playback.Add(i * 30.0, At({static_cast<float>(i), 0.f, 0.f}), false);
    CHECK(playback.GetCount() == Playback::kUpdates);
    NearPosition(playback.At(0.0, 0.0).Position, {20.f - Playback::kUpdates, 0.f, 0.f}, 1e-6f);

    playback.Add(5.0, At({99.f, 0.f, 0.f}), false);
    CHECK(playback.GetCount() == 1);
}

TEST_CASE("a put-down is reached once the shown moment gets to it", "[heldobjects]")
{
    Playback playback;
    playback.Add(0.0, At({0.f, 0.f, 0.f}), false);
    playback.Add(40.0, At({4.f, 0.f, 0.f}), true);
    CHECK_FALSE(playback.IsRestShown(60.0, 50.0));
    CHECK(playback.IsRestShown(90.0, 50.0));
    playback.Clear();
    CHECK_FALSE(playback.IsRestShown(90.0, 50.0));
}

TEST_CASE("an object that keeps its place against a hand is drawn from that hand", "[heldobjects]")
{
    Grip grip;
    constexpr uint64_t cHand = 7;
    const glm::vec3 cOffset(5.f, 2.f, -3.f);
    double now = 0.0;
    Pose hand = At({100.f, 0.f, 50.f}, 0.3f);

    // Carried with the hand: steady against it for the settle time, then held.
    for (; now < Grip::kSteadyFor; now += 16.0)
    {
        hand.Position.x += 1.f;
        const Pose cPath{hand.Position + hand.Rotation * cOffset, hand.Rotation};
        grip.Update(now, cHand, &hand, cPath);
        CHECK_FALSE(grip.IsHeld());
    }
    hand.Position.x += 1.f;
    const Pose cHeldAt = grip.Update(now, cHand, &hand, {hand.Position + hand.Rotation * cOffset, hand.Rotation});
    REQUIRE(grip.IsHeld());
    CHECK(grip.GetHandKey() == cHand);
    CHECK(grip.GetGripDistance() == Approx(glm::length(cOffset)).margin(1e-3));
    // Taking hold of it does not move it.
    NearPosition(cHeldAt.Position, hand.Position + hand.Rotation * cOffset, 1e-3f);

    // The updates lag the hand by 8 units (the clocks differ a little): drawn at the hand, not where the updates are.
    now += 16.0;
    hand.Position.y += 10.f;
    hand.Rotation = glm::angleAxis(0.6f, glm::vec3(0.f, 0.f, 1.f));
    const Pose cLagging{hand.Position + hand.Rotation * cOffset - glm::vec3(0.f, 8.f, 0.f), hand.Rotation};
    const Pose cDrawn = grip.Update(now, cHand, &hand, cLagging);
    CHECK(grip.IsHeld());
    NearPosition(cDrawn.Position, hand.Position + hand.Rotation * cOffset, 1e-3f);
    CHECK(AngleBetween(cDrawn.Rotation, hand.Rotation) < 3e-3f);
}

TEST_CASE("an object leaves the hand when the updates move it away, without a jump", "[heldobjects]")
{
    Grip grip;
    const Pose cHand = At({0.f, 0.f, 0.f});
    const Pose cInHand = At({3.f, 0.f, 0.f});
    double now = 0.0;
    for (; !grip.IsHeld() && now < 1000.0; now += 16.0)
        grip.Update(now, 1, &cHand, cInHand);
    REQUIRE(grip.IsHeld());

    // Thrown: the updates fly off. Apart for less than kApartFor is a hiccup; kApartFor in a row is a let-go.
    const Pose cThrown = At({40.f, 0.f, 0.f});
    const double cApartAt = now;
    for (; now - cApartAt < Grip::kApartFor; now += 16.0)
    {
        NearPosition(grip.Update(now, 1, &cHand, cThrown).Position, cInHand.Position, 1e-4f);
        CHECK(grip.IsHeld());
    }
    const Pose cLetGo = grip.Update(now, 1, &cHand, cThrown);
    CHECK_FALSE(grip.IsHeld());
    // Blended from the hand onto the updates, then on them.
    NearPosition(cLetGo.Position, cInHand.Position, 1e-3f);
    const Pose cHalfway = grip.Update(now + Grip::kBlendFor / 2.0, 0, nullptr, cThrown);
    NearPosition(cHalfway.Position, (cInHand.Position + cThrown.Position) * 0.5f, 1e-2f);
    NearPosition(grip.Update(now + Grip::kBlendFor, 0, nullptr, cThrown).Position, cThrown.Position, 1e-4f);
}

namespace
{
// Holds an object 3 units along x from a hand that moves aStep a frame (16 ms) from the origin, until it is held and for
// aHeldFor ms more; then the updates fly off to aThrownTo while the hand goes on, until it is let go. Returns where it was
// let go, its hand's speed then, and the time.
struct Throw
{
    Pose LetGo{};
    glm::vec3 Velocity{};
    double At{};
};
Throw SwingAndThrow(Grip& aGrip, const glm::vec3& acStep, const double aHeldFor, const Pose& acThrownTo)
{
    Pose hand = At({0.f, 0.f, 0.f});
    const glm::vec3 cOffset(3.f, 0.f, 0.f);
    double now = 0.0;
    for (; !aGrip.IsHeld() && now < 1000.0; now += 16.0)
    {
        hand.Position += acStep;
        aGrip.Update(now, 1, &hand, {hand.Position + cOffset, hand.Rotation});
    }
    REQUIRE(aGrip.IsHeld());
    for (const double cHeldAt = now; now - cHeldAt < aHeldFor; now += 16.0)
    {
        hand.Position += acStep;
        aGrip.Update(now, 1, &hand, {hand.Position + cOffset, hand.Rotation});
    }
    Throw result{};
    for (const double cApartAt = now; aGrip.IsHeld(); now += 16.0)
    {
        hand.Position += acStep;
        if (aGrip.IsHeld())
            result.Velocity = aGrip.GetHeldVelocity();
        result.LetGo = aGrip.Update(now, 1, &hand, acThrownTo);
        result.At = now;
        REQUIRE(now - cApartAt <= Grip::kApartFor + 16.0);
    }
    return result;
}
} // namespace

TEST_CASE("a thrown object leaves the hand at the hand's speed, then joins its updates", "[heldobjects]")
{
    Grip grip;
    // A swing along y at 625 units a second; the updates fly well ahead.
    const Pose cThrown = At({3.f, 400.f, 0.f});
    const Throw cThrow = SwingAndThrow(grip, {0.f, 10.f, 0.f}, 100.0, cThrown);
    CHECK_FALSE(grip.IsHeld());
    CHECK(cThrow.Velocity.y == Approx(625.f).margin(10.f));
    CHECK(std::fabs(cThrow.Velocity.x) < 1e-3f);

    // Halfway through the blend: from where the hand would have carried it, not from where it let go.
    const double cHalf = Grip::kBlendFor / 2.0;
    const glm::vec3 cCarried = cThrow.LetGo.Position + cThrow.Velocity * static_cast<float>(cHalf / 1000.0);
    const Pose cHalfway = grip.Update(cThrow.At + cHalf, 0, nullptr, cThrown);
    NearPosition(cHalfway.Position, (cCarried + cThrown.Position) * 0.5f, 1e-2f);
    // Ahead of a blend from a standstill by half the hand's travel.
    const float cAhead = cHalfway.Position.y - (cThrow.LetGo.Position.y + cThrown.Position.y) * 0.5f;
    CHECK(cAhead == Approx(0.5f * 625.f * static_cast<float>(cHalf / 1000.0)).margin(1.f));
    // Then on the updates.
    NearPosition(grip.Update(cThrow.At + Grip::kBlendFor, 0, nullptr, cThrown).Position, cThrown.Position, 1e-4f);
}

TEST_CASE("a let-go never carries the object faster than a hard throw", "[heldobjects]")
{
    Grip grip;
    // 100 units a frame is over 6000 units a second: a hitch or a snap, carried at the cap only.
    const Pose cThrown = At({3.f, 5000.f, 0.f});
    const Throw cThrow = SwingAndThrow(grip, {0.f, 100.f, 0.f}, 100.0, cThrown);
    CHECK(glm::length(cThrow.Velocity) > Grip::kMaxReleaseSpeed);
    const double cHalf = Grip::kBlendFor / 2.0;
    const Pose cHalfway = grip.Update(cThrow.At + cHalf, 0, nullptr, cThrown);
    const float cAhead = cHalfway.Position.y - (cThrow.LetGo.Position.y + cThrown.Position.y) * 0.5f;
    CHECK(cAhead == Approx(0.5f * Grip::kMaxReleaseSpeed * static_cast<float>(cHalf / 1000.0)).margin(1.f));
}

TEST_CASE("an object let go from a still hand starts from where it was", "[heldobjects]")
{
    Grip grip;
    const Pose cThrown = At({40.f, 0.f, 0.f});
    const Throw cThrow = SwingAndThrow(grip, {0.f, 0.f, 0.f}, 50.0, cThrown);
    CHECK(glm::length(cThrow.Velocity) < 1e-4f);
    const Pose cHalfway = grip.Update(cThrow.At + Grip::kBlendFor / 2.0, 0, nullptr, cThrown);
    NearPosition(cHalfway.Position, (cThrow.LetGo.Position + cThrown.Position) * 0.5f, 1e-2f);
}

TEST_CASE("objects away from a hand, wobbling in it, or passed to another hand are not drawn from it", "[heldobjects]")
{
    const Pose cHand = At({0.f, 0.f, 0.f});
    {
        Grip grip;
        for (double now = 0.0; now < 1000.0; now += 16.0)
            grip.Update(now, 1, &cHand, At({Grip::kReach + 1.f, 0.f, 0.f}));
        CHECK_FALSE(grip.IsHeld());
    }
    {
        // A copy whose hand plays its own animation: the object never keeps a place against it.
        Grip grip;
        int frame = 0;
        for (double now = 0.0; now < 1000.0; now += 16.0, ++frame)
            grip.Update(now, 1, &cHand, At({10.f + (frame % 2 ? 3.f : -3.f), 0.f, 0.f}));
        CHECK_FALSE(grip.IsHeld());
    }
    {
        Grip grip;
        double now = 0.0;
        for (; !grip.IsHeld() && now < 1000.0; now += 16.0)
            grip.Update(now, 1, &cHand, At({3.f, 0.f, 0.f}));
        REQUIRE(grip.IsHeld());
        // The holding hand is gone and another one is nearest: let go after kApartFor, and held by that one only
        // once steady against it.
        const double cGoneAt = now;
        for (; now - cGoneAt < Grip::kApartFor; now += 16.0)
        {
            grip.Update(now, 2, &cHand, At({3.f, 0.f, 0.f}));
            CHECK(grip.IsHeld());
        }
        grip.Update(now, 2, &cHand, At({3.f, 0.f, 0.f}));
        CHECK_FALSE(grip.IsHeld());
        grip.LetGo(now);
        CHECK_FALSE(grip.IsHeld());
        CHECK(grip.GetHandKey() == 0u);
    }
}

TEST_CASE("a held object outlasts a missing hand frame and never takes a broken hand", "[heldobjects]")
{
    const Pose cHand = At({0.f, 0.f, 0.f});
    const Pose cInHand = At({3.f, 0.f, 0.f});
    Grip grip;
    double now = 0.0;
    for (; !grip.IsHeld() && now < 1000.0; now += 16.0)
        grip.Update(now, 1, &cHand, cInHand);
    REQUIRE(grip.IsHeld());

    // One frame the hand was not posed: it stays where the hand last held it.
    NearPosition(grip.Update(now, 0, nullptr, cInHand).Position, cInHand.Position, 1e-4f);
    CHECK(grip.IsHeld());
    now += 16.0;
    NearPosition(grip.Update(now, 1, &cHand, cInHand).Position, cInHand.Position, 1e-4f);
    CHECK(grip.IsHeld());

    // A hand read mid-write counts as missing: never drawn from, and let go after kApartFor.
    const float cNan = std::numeric_limits<float>::quiet_NaN();
    const Pose cBroken = At({cNan, 0.f, 0.f});
    for (double brokenFor = 0.0; brokenFor <= Grip::kApartFor + 16.0; brokenFor += 16.0)
    {
        now += 16.0;
        CHECK(IsFinite(grip.Update(now, 1, &cBroken, cInHand)));
    }
    CHECK_FALSE(grip.IsHeld());
    for (int frame = 0; frame < 30; ++frame)
    {
        now += 16.0;
        CHECK(IsFinite(grip.Update(now, 1, &cBroken, cInHand)));
        CHECK_FALSE(grip.IsHeld());
    }
}

TEST_CASE("a change of delay is eased in, never jumped", "[heldobjects]")
{
    Delay delay;
    // The first wanted delay is taken as it is.
    CHECK(delay.Follow(1000.0, 130.0) == Approx(130.0));
    // The wanted delay drops to 50: it moves a tenth of the time passed per frame, so the object never skips ahead.
    CHECK(delay.Follow(1016.0, 50.0) == Approx(130.0 - 1.6));
    CHECK(delay.Follow(1032.0, 50.0) == Approx(130.0 - 3.2));
    double shown = 0.0;
    for (double now = 1048.0; now < 3000.0; now += 16.0)
        shown = delay.Follow(now, 50.0);
    CHECK(shown == Approx(50.0));
    // Rising again, it eases up the same way: the object slows a little rather than stepping back.
    CHECK(delay.Follow(3000.0, 200.0) == Approx(51.6));
}

TEST_CASE("an object is let go after a tenth of a second whatever the frame rate", "[heldobjects]")
{
    CHECK(Grip::kApartFor == Approx(100.0));
    for (const double cFrame : {1000.0 / 144.0, 1000.0 / 90.0, 1000.0 / 30.0})
    {
        Grip grip;
        const Pose cHand = At({0.f, 0.f, 0.f});
        double now = 0.0;
        for (; !grip.IsHeld() && now < 1000.0; now += cFrame)
            grip.Update(now, 1, &cHand, At({3.f, 0.f, 0.f}));
        REQUIRE(grip.IsHeld());

        // The updates fly off: held until a tenth of a second has passed, let go on the first frame after.
        const double cApartAt = now;
        const Pose cThrown = At({40.f, 0.f, 0.f});
        for (; now - cApartAt < Grip::kApartFor; now += cFrame)
        {
            grip.Update(now, 1, &cHand, cThrown);
            CHECK(grip.IsHeld());
        }
        grip.Update(now, 1, &cHand, cThrown);
        CHECK_FALSE(grip.IsHeld());
    }
}

TEST_CASE("a moment of the updates coming back cancels a let-go", "[heldobjects]")
{
    Grip grip;
    const Pose cHand = At({0.f, 0.f, 0.f});
    double now = 0.0;
    for (; !grip.IsHeld() && now < 1000.0; now += 16.0)
        grip.Update(now, 1, &cHand, At({3.f, 0.f, 0.f}));
    REQUIRE(grip.IsHeld());

    // Apart for 80 ms, back for a frame, apart for 80 ms again: never apart for 100 ms in a row.
    for (int round = 0; round < 2; ++round)
    {
        for (double apartFor = 0.0; apartFor < 80.0; apartFor += 16.0, now += 16.0)
            grip.Update(now, 1, &cHand, At({40.f, 0.f, 0.f}));
        grip.Update(now, 1, &cHand, At({3.f, 0.f, 0.f}));
        now += 16.0;
        CHECK(grip.IsHeld());
    }
}

TEST_CASE("a hand is put a frame ahead by the step it just took, within a cap", "[heldobjects]")
{
    HandLead lead;
    // The first read has nothing to go by.
    NearPosition(lead.Lead(0.0, At({0.f, 0.f, 0.f})).Position, {0.f, 0.f, 0.f}, 1e-5f);
    // A steady 3 units a frame: the next frame is 3 further on.
    NearPosition(lead.Lead(11.0, At({3.f, 0.f, 0.f})).Position, {6.f, 0.f, 0.f}, 1e-4f);
    NearPosition(lead.Lead(22.0, At({6.f, 0.f, 0.f})).Position, {9.f, 0.f, 0.f}, 1e-4f);
    // A still hand stays where it is.
    NearPosition(lead.Lead(33.0, At({6.f, 0.f, 0.f})).Position, {6.f, 0.f, 0.f}, 1e-4f);
    NearPosition(lead.Lead(44.0, At({6.f, 0.f, 0.f})).Position, {6.f, 0.f, 0.f}, 1e-4f);
}

TEST_CASE("a hand's lead never passes the cap, and never follows a hitch or a broken read", "[heldobjects]")
{
    HandLead lead;
    lead.Lead(0.0, At({0.f, 0.f, 0.f}));
    // A jump of 100 units in a frame leads by the cap only.
    const Pose cJump = lead.Lead(11.0, At({100.f, 0.f, 0.f}));
    CHECK(glm::distance(cJump.Position, glm::vec3(100.f, 0.f, 0.f)) == Approx(HandLead::kMaxLead));
    // After a hitch the step says nothing about the next frame.
    NearPosition(lead.Lead(11.0 + HandLead::kMaxGap + 1.0, At({200.f, 0.f, 0.f})).Position, {200.f, 0.f, 0.f}, 1e-4f);
    // A read at the same time as the last (a second update in a frame) does not lead.
    NearPosition(lead.Lead(11.0 + HandLead::kMaxGap + 1.0, At({210.f, 0.f, 0.f})).Position, {210.f, 0.f, 0.f}, 1e-4f);
    // The lead is always finite for finite reads.
    CHECK(IsFinite(lead.Lead(100.0, At({1.f, 2.f, 3.f}, 1.f))));
    CHECK(lead.GetLastRead() == Approx(100.0));
}

TEST_CASE("a hand's turn is led by its last step, within a cap", "[heldobjects]")
{
    HandLead lead;
    lead.Lead(0.0, At({0.f, 0.f, 0.f}, 0.f));
    // A small turn of 0.05 rad a frame leads by the same again.
    const Pose cSmall = lead.Lead(11.0, At({0.f, 0.f, 0.f}, 0.05f));
    CHECK(AngleBetween(cSmall.Rotation, glm::angleAxis(0.10f, glm::vec3(0.f, 0.f, 1.f))) < 2e-3f);
    // A spin of 1 rad in a frame leads by the cap only.
    const Pose cSpin = lead.Lead(22.0, At({0.f, 0.f, 0.f}, 1.05f));
    CHECK(AngleBetween(cSpin.Rotation, glm::angleAxis(1.05f, glm::vec3(0.f, 0.f, 1.f))) == Approx(HandLead::kMaxTurn).margin(2e-3));
}

TEST_CASE("a late object is carried on briefly, then held, and the update that corrects it is eased in", "[heldobjects]")
{
    Playback playback;
    playback.Add(0.0, At({0.f, 0.f, 0.f}), false);
    playback.Add(40.0, At({4.f, 0.f, 0.f}), false);

    // Nothing newer by the shown moment 80: goes on at 0.1 units a ms, 4 units for the 40 ms.
    NearPosition(playback.Show(80.0, 0.0).Position, {8.f, 0.f, 0.f}, 1e-4f);
    // For no longer than kExtrapolate, then held.
    NearPosition(playback.Show(40.0 + Playback::kExtrapolate, 0.0).Position, {4.f + 0.1f * static_cast<float>(Playback::kExtrapolate), 0.f, 0.f}, 1e-3f);
    NearPosition(playback.Show(1000.0, 0.0).Position, {4.f + 0.1f * static_cast<float>(Playback::kExtrapolate), 0.f, 0.f}, 1e-3f);

    // The late update says it was at 7 at moment 80, not 8: the picture starts from where it was and eases there.
    Playback late;
    late.Add(0.0, At({0.f, 0.f, 0.f}), false);
    late.Add(40.0, At({4.f, 0.f, 0.f}), false);
    NearPosition(late.Show(80.0, 0.0).Position, {8.f, 0.f, 0.f}, 1e-4f);
    late.Add(80.0, At({7.f, 0.f, 0.f}), false);
    NearPosition(late.Show(80.0, 0.0).Position, {8.f, 0.f, 0.f}, 1e-4f);
    // No frame moves backward by more than the error, and none jumps: halfway through the blend the error is half gone.
    const Pose cHalf = late.Show(80.0 + Playback::kCorrectFor / 2.0, 0.0);
    CHECK(cHalf.Position.x > 7.f);
    // Once the blend is over, what is shown is the path itself.
    // (Carried on from 7 at 0.075 units a ms, for the 100 ms it is allowed.)
    NearPosition(late.Show(80.0 + Playback::kCorrectFor + 1.0, 0.0).Position, {7.f + 0.075f * static_cast<float>(Playback::kExtrapolate), 0.f, 0.f}, 1e-3f);
}

TEST_CASE("a put-down is never carried on past, and a far correction is not blended", "[heldobjects]")
{
    Playback rest;
    rest.Add(0.0, At({0.f, 0.f, 0.f}), false);
    rest.Add(40.0, At({4.f, 0.f, 0.f}), true);
    NearPosition(rest.Show(100.0, 0.0).Position, {4.f, 0.f, 0.f}, 1e-5f);
    CHECK(rest.IsRestShown(100.0, 0.0));
    NearPosition(rest.Newest().Position, {4.f, 0.f, 0.f}, 1e-6f);

    // A correction of more than kCorrectMax is a jump the sender made (a catch, a teleport): taken as it is.
    Playback jump;
    jump.Add(0.0, At({0.f, 0.f, 0.f}), false);
    jump.Add(40.0, At({4.f, 0.f, 0.f}), false);
    jump.Show(80.0, 0.0);
    jump.Add(80.0, At({-200.f, 0.f, 0.f}), false);
    NearPosition(jump.Show(80.0, 0.0).Position, {-200.f, 0.f, 0.f}, 1e-3f);
}

TEST_CASE("an update goes at its sender's tick, one without goes by the network's delay", "[heldobjects]")
{
    Timeline line;
    // No tick yet: arrival times.
    CHECK(line.Note(1000.0, 0) == Approx(1000.0));
    CHECK_FALSE(line.IsTimed());

    CHECK(line.Note(1000.0, 5000) == Approx(5000.0));
    REQUIRE(line.IsTimed());
    // A resent resting place carries none: at the tick it could have been sent at, 100 ms after the last one's.
    CHECK(line.Note(1100.0, 0) == Approx(5100.0));
}

TEST_CASE("a throw is shown at the sender's speed whatever the network does to the packets", "[heldobjects]")
{
    // The sender sends every 33 ms, 10 units further each time (0.303 units a ms). The packets take 40 ms and up to 30 ms
    // more, unevenly. On arrival times the speed would swing with the jitter; on ticks it stays the sender's.
    constexpr double cInterval = 33.0;
    constexpr uint64_t cTickBase = 5000000;
    constexpr double cArrivalBase = 123.0;
    constexpr double cSpeed = 10.0 / cInterval;

    Timeline line;
    Playback playback;
    int next = 0;
    double previousX = 0.0;
    bool havePrevious = false;
    int checked = 0;
    for (double now = 0.0; now < 4000.0; now += 11.0)
    {
        // Packets that have arrived by now, in order.
        for (;; ++next)
        {
            const double cJitter = static_cast<double>((next * 7919) % 31);
            const double cArrival = cArrivalBase + next * cInterval + 40.0 + cJitter;
            if (cArrival > now)
                break;
            const uint64_t cTick = cTickBase + static_cast<uint64_t>(next * cInterval);
            const double cTime = line.Note(cArrival, cTick);
            playback.Add(cTime, At({static_cast<float>(next * 10), 0.f, 0.f}), false);
        }
        if (!line.IsTimed())
            continue;
        const double cX = playback.Show(line.Moment(now, 0.0), 0.0).Position.x;
        if (havePrevious && now > 1500.0)
        {
            // At most the clock's 10% to catch up, and no more.
            const double cPerMs = (cX - previousX) / 11.0;
            CHECK(cPerMs == Approx(cSpeed).epsilon(0.12));
            ++checked;
        }
        previousX = cX;
        havePrevious = true;
    }
    CHECK(checked > 100);
}

TEST_CASE("a late update that is still late is corrected against the path that is drawn, not its end", "[heldobjects]")
{
    // Moving at 0.1 units a ms. At 150 the picture is carried on to 15; the update for 120 says 12, on the same line.
    Playback playback;
    playback.Add(0.0, At({0.f, 0.f, 0.f}), false);
    playback.Add(100.0, At({10.f, 0.f, 0.f}), false);
    NearPosition(playback.Show(150.0, 0.0).Position, {15.f, 0.f, 0.f}, 1e-4f);
    playback.Add(120.0, At({12.f, 0.f, 0.f}), false);
    // Nothing to correct: the same 15, then on at the same speed, never 18.
    NearPosition(playback.Show(150.0, 0.0).Position, {15.f, 0.f, 0.f}, 1e-4f);
    NearPosition(playback.Show(170.0, 0.0).Position, {17.f, 0.f, 0.f}, 1e-4f);

    // Late twice at the same speed: still nothing to correct.
    playback.Add(140.0, At({14.f, 0.f, 0.f}), false);
    NearPosition(playback.Show(170.0, 0.0).Position, {17.f, 0.f, 0.f}, 1e-4f);
    NearPosition(playback.Show(190.0, 0.0).Position, {19.f, 0.f, 0.f}, 1e-4f);
}

TEST_CASE("a late update that slows a throw down twice is eased in without a jump", "[heldobjects]")
{
    Playback playback;
    playback.Add(0.0, At({0.f, 0.f, 0.f}), false);
    playback.Add(100.0, At({10.f, 0.f, 0.f}), false);
    NearPosition(playback.Show(150.0, 0.0).Position, {15.f, 0.f, 0.f}, 1e-4f);

    // It slowed to 0.05 a ms: the path at 150 is 12.5, and the 2.5 drawn ahead of it fades out.
    playback.Add(120.0, At({11.f, 0.f, 0.f}), false);
    NearPosition(playback.Show(150.0, 0.0).Position, {15.f, 0.f, 0.f}, 1e-4f);
    const glm::vec3 cBefore = playback.Show(160.0, 0.0).Position;
    NearPosition(cBefore, {13.f + 2.5f * 0.9f, 0.f, 0.f}, 1e-4f);

    // Late again, while that fades: what was drawn stays put, and the rest of the difference fades from there.
    playback.Add(140.0, At({12.f, 0.f, 0.f}), false);
    NearPosition(playback.Show(160.0, 0.0).Position, cBefore, 1e-4f);
    // Once faded, the path alone: carried on from 12 at 0.05 a ms for the 100 ms allowed.
    NearPosition(playback.Show(160.0 + Playback::kCorrectFor, 0.0).Position, {17.f, 0.f, 0.f}, 1e-4f);
}

TEST_CASE("a late update that turns a throw back eases from the drawn place onto the new way", "[heldobjects]")
{
    Playback playback;
    playback.Add(0.0, At({0.f, 0.f, 0.f}), false);
    playback.Add(100.0, At({10.f, 0.f, 0.f}), false);
    NearPosition(playback.Show(150.0, 0.0).Position, {15.f, 0.f, 0.f}, 1e-4f);

    // Back at 0.1 a ms from 120: the path at 150 is 5, 10 behind what was drawn.
    playback.Add(120.0, At({8.f, 0.f, 0.f}), false);
    NearPosition(playback.Show(150.0, 0.0).Position, {15.f, 0.f, 0.f}, 1e-4f);
    NearPosition(playback.Show(200.0, 0.0).Position, {0.f + 5.f, 0.f, 0.f}, 1e-4f);
    // Faded: carried back for the 100 ms allowed past 120, to -2.
    NearPosition(playback.Show(150.0 + Playback::kCorrectFor, 0.0).Position, {-2.f, 0.f, 0.f}, 1e-4f);
}

TEST_CASE("a late put-down eases from the drawn place to where it was put, and stays", "[heldobjects]")
{
    Playback playback;
    playback.Add(0.0, At({0.f, 0.f, 0.f}), false);
    playback.Add(100.0, At({10.f, 0.f, 0.f}), false);
    NearPosition(playback.Show(150.0, 0.0).Position, {15.f, 0.f, 0.f}, 1e-4f);

    playback.Add(120.0, At({12.f, 0.f, 0.f}), true);
    NearPosition(playback.Show(150.0, 0.0).Position, {15.f, 0.f, 0.f}, 1e-4f);
    NearPosition(playback.Show(200.0, 0.0).Position, {12.f + 1.5f, 0.f, 0.f}, 1e-4f);
    NearPosition(playback.Show(150.0 + Playback::kCorrectFor, 0.0).Position, {12.f, 0.f, 0.f}, 1e-4f);
    NearPosition(playback.Show(1000.0, 0.0).Position, {12.f, 0.f, 0.f}, 1e-6f);
    CHECK(playback.IsRestShown(1000.0, 0.0));
}

TEST_CASE("a correction left over is dropped when the updates start again", "[heldobjects]")
{
    Playback playback;
    playback.Add(0.0, At({0.f, 0.f, 0.f}), false);
    playback.Add(100.0, At({10.f, 0.f, 0.f}), false);
    playback.Show(150.0, 0.0);
    playback.Add(120.0, At({8.f, 0.f, 0.f}), false);
    playback.Show(150.0, 0.0);

    // Out of order: a new line, with nothing of the old correction in it.
    playback.Add(50.0, At({100.f, 0.f, 0.f}), false);
    NearPosition(playback.Show(60.0, 0.0).Position, {100.f, 0.f, 0.f}, 1e-6f);

    // The same after a Clear (the first tick of a throw).
    Playback cleared;
    cleared.Add(0.0, At({0.f, 0.f, 0.f}), false);
    cleared.Add(100.0, At({10.f, 0.f, 0.f}), false);
    cleared.Show(150.0, 0.0);
    cleared.Add(120.0, At({8.f, 0.f, 0.f}), false);
    cleared.Show(150.0, 0.0);
    cleared.Clear();
    cleared.Add(150.0, At({50.f, 0.f, 0.f}), false);
    NearPosition(cleared.Show(160.0, 0.0).Position, {50.f, 0.f, 0.f}, 1e-6f);
}

TEST_CASE("a speed is capped, and one that is not a number is none", "[heldobjects]")
{
    NearPosition(CapSpeed({300.f, 0.f, 400.f}, 1000.f), {300.f, 0.f, 400.f}, 1e-4f);
    CHECK(glm::length(CapSpeed({3000.f, 0.f, 4000.f}, 1000.f)) == Approx(1000.f));
    NearPosition(CapSpeed({3000.f, 0.f, 4000.f}, 1000.f), {600.f, 0.f, 800.f}, 1e-3f);
    const float cNan = std::numeric_limits<float>::quiet_NaN();
    NearPosition(CapSpeed({cNan, 0.f, 0.f}, 1000.f), {0.f, 0.f, 0.f}, 0.f);

    const Flight cWild = Capped({{1e6f, 0.f, 0.f}, {0.f, 0.f, 1e3f}});
    CHECK(glm::length(cWild.Velocity) == Approx(kMaxThrowSpeed));
    CHECK(glm::length(cWild.Spin) == Approx(kMaxThrowSpin));
}

TEST_CASE("a flight is measured from two polls of the object, not from polls too close or too far apart", "[heldobjects]")
{
    const Pose cFrom = At({0.f, 0.f, 0.f});
    const Pose cTo = At({10.f, 0.f, -5.f}, 0.1f);
    const Flight cFlight = FlightBetween(cFrom, cTo, 33.0);
    NearPosition(cFlight.Velocity, glm::vec3{10.f, 0.f, -5.f} / 0.033f, 0.05f);
    // Turned 0.1 rad about z in 33 ms.
    NearPosition(cFlight.Spin, {0.f, 0.f, 0.1f / 0.033f}, 1e-2f);

    for (const double cGap : {0.0, kMinPollGap - 1.0, kMaxPollGap + 1.0, -33.0})
    {
        const Flight cNone = FlightBetween(cFrom, cTo, cGap);
        NearPosition(cNone.Velocity, {0.f, 0.f, 0.f}, 0.f);
        NearPosition(cNone.Spin, {0.f, 0.f, 0.f}, 0.f);
    }
    // A jump between two polls is a hard throw at most.
    CHECK(glm::length(FlightBetween(cFrom, At({1000.f, 0.f, 0.f}), 33.0).Velocity) == Approx(kMaxThrowSpeed));
}

TEST_CASE("an object let go by a hand is thrown, one never held or let go long ago is not", "[heldobjects]")
{
    Release release;
    double now = 0.0;
    // Carried, then let go: thrown from the second free poll on.
    for (; now < 300.0; now += 33.0)
        CHECK_FALSE(release.Poll(now, true, true));
    CHECK_FALSE(release.Poll(now, false, true));
    now += 33.0;
    CHECK(release.Poll(now, false, true));
    CHECK(release.IsThrown());
    now += 33.0;
    CHECK(release.Poll(now, false, true));

    // Caught again: no longer thrown.
    now += 33.0;
    CHECK_FALSE(release.Poll(now, true, true));
    CHECK_FALSE(release.IsThrown());

    // One poll without the hand is not a let-go.
    now += 33.0;
    CHECK_FALSE(release.Poll(now, false, true));
    now += 33.0;
    CHECK_FALSE(release.Poll(now, true, true));

    // Put down: the next move starts from no hold.
    release.Rest();
    CHECK_FALSE(release.IsThrown());
    for (int i = 0; i < 5; ++i)
    {
        now += 33.0;
        CHECK_FALSE(release.Poll(now, false, true));
    }

    // Pushed: never held.
    Release pushed;
    for (double t = 0.0; t < 500.0; t += 33.0)
        CHECK_FALSE(pushed.Poll(t, false, true));

    // Held, then left lying until a hand pushed it much later: not thrown.
    Release late;
    late.Poll(0.0, true, true);
    CHECK_FALSE(late.Poll(Release::kHeldWithin + 100.0, false, true));
    CHECK_FALSE(late.Poll(Release::kHeldWithin + 133.0, false, true));
    CHECK_FALSE(late.Poll(Release::kHeldWithin + 166.0, false, true));
}

TEST_CASE("an object at rest near a hand is never carried or thrown", "[heldobjects]")
{
    Carry carry;
    Release release;
    Carry::Hands hands{};
    const glm::vec3 cObject{0.f, 0.f, 0.f};
    double now = 0.0;
    // A hand reaches past it, through it even, and away.
    for (float x = -100.f; x <= 100.f; x += 10.f, now += 33.0)
    {
        hands[0] = {{x, 5.f, 0.f}, {10.f, 0.f, 0.f}, true};
        const bool cHeld = carry.Poll(hands, cObject);
        CHECK_FALSE(cHeld);
        CHECK_FALSE(release.Poll(now, cHeld, false));
    }
    // Then something else pushes it, the hand far off: still not thrown.
    glm::vec3 pushed = cObject;
    for (int i = 0; i < 10; ++i, now += 33.0)
    {
        pushed.x += 5.f;
        hands[0] = {{300.f, 0.f, 0.f}, {}, true};
        const bool cHeld = carry.Poll(hands, pushed);
        CHECK_FALSE(cHeld);
        CHECK_FALSE(release.Poll(now, cHeld, true));
    }
}

TEST_CASE("an object that moves with a hand is carried, and thrown when let go", "[heldobjects]")
{
    Carry carry;
    Release release;
    Carry::Hands hands{};
    glm::vec3 hand{0.f, 0.f, 100.f};
    glm::vec3 object{10.f, 0.f, 100.f};
    double now = 0.0;
    // The first poll has no step to go by.
    hands[1] = {hand, {}, true};
    CHECK_FALSE(carry.Poll(hands, object));
    // Lifted with the hand: carried once it has moved with it kTogetherPolls in a row.
    for (uint32_t i = 1; i <= Carry::kTogetherPolls; ++i)
    {
        now += 33.0;
        hand.z += 5.f;
        object.z += 5.f;
        hands[1] = {hand, {0.f, 0.f, 5.f}, true};
        CHECK(carry.Poll(hands, object) == (i == Carry::kTogetherPolls));
        CHECK_FALSE(release.Poll(now, carry.IsHeld(), true));
    }
    // Swung, turning a little in the hand: still carried.
    for (int i = 0; i < 5; ++i)
    {
        now += 33.0;
        hand.x += 10.f;
        object.x += 13.f;
        object.y += 2.f;
        hands[1] = {hand, {10.f, 0.f, 0.f}, true};
        CHECK(carry.Poll(hands, object));
        CHECK_FALSE(release.Poll(now, true, true));
    }
    // Let go: it flies on while the hand stops, and is thrown from the second poll out of the hand.
    int thrownAt = -1;
    for (int i = 0; i < 4 && thrownAt < 0; ++i)
    {
        now += 33.0;
        object.x += 50.f;
        hands[1] = {hand, {}, true};
        if (release.Poll(now, carry.Poll(hands, object), true))
            thrownAt = i;
    }
    CHECK(thrownAt == 1);
    CHECK_FALSE(carry.IsHeld());
}

TEST_CASE("a long object carried by its far end is not let go while it swings around the reach", "[heldobjects]")
{
    Carry carry;
    Release release;
    Carry::Hands hands{};
    glm::vec3 hand{0.f, 0.f, 0.f};
    double now = 0.0;
    const auto cPoll = [&](const glm::vec3& acHandStep, const glm::vec3& acOffset)
    {
        now += 33.0;
        hand += acHandStep;
        hands[0] = {hand, acHandStep, true};
        const bool cHeld = carry.Poll(hands, hand + acOffset);
        return std::pair{cHeld, release.Poll(now, cHeld, true)};
    };
    // Picked up 35 units from its root.
    cPoll({}, {35.f, 0.f, 0.f});
    for (uint32_t i = 0; i < Carry::kTogetherPolls; ++i)
        cPoll({0.f, 0.f, 5.f}, {35.f, 0.f, 0.f});
    REQUIRE(carry.IsHeld());
    // Carried and turned: its root swings between 38 and 55 units from the hand.
    for (int i = 0; i < 30; ++i)
    {
        const float cReach = i % 2 ? 55.f : 38.f;
        const auto [cHeld, cThrown] = cPoll({8.f, 0.f, 0.f}, {0.f, cReach, 0.f});
        CHECK(cHeld);
        CHECK_FALSE(cThrown);
    }
    // Beyond the let-go reach: out of the hand.
    CHECK_FALSE(cPoll({}, {0.f, Carry::kLetGoReach + 20.f, 0.f}).first);
    CHECK(Carry::kLetGoReach > Grip::kReach);
}

TEST_CASE("a hand that hits an object away does not carry it", "[heldobjects]")
{
    Carry carry;
    Release release;
    Carry::Hands hands{};
    glm::vec3 hand{-60.f, 0.f, 0.f};
    glm::vec3 object{0.f, 0.f, 0.f};
    double now = 0.0;
    hands[0] = {hand, {}, true};
    carry.Poll(hands, object);
    // The hand comes in fast; the object waits.
    for (int i = 0; i < 3; ++i, now += 33.0)
    {
        hand.x += 20.f;
        hands[0] = {hand, {20.f, 0.f, 0.f}, true};
        CHECK_FALSE(carry.Poll(hands, object));
        CHECK_FALSE(release.Poll(now, false, false));
    }
    // Hit: it flies off far faster than the hand follows through.
    for (int i = 0; i < 6; ++i, now += 33.0)
    {
        hand.x += 5.f;
        object.x += 60.f;
        hands[0] = {hand, {5.f, 0.f, 0.f}, true};
        const bool cHeld = carry.Poll(hands, object);
        CHECK_FALSE(cHeld);
        CHECK_FALSE(release.Poll(now, cHeld, true));
    }
}

TEST_CASE("a throw is forgotten once the object rests or another player takes it", "[heldobjects]")
{
    double now = 0.0;
    const auto cThrow = [&now](Release& aRelease)
    {
        aRelease.Poll(now, true, true);
        now += 33.0;
        aRelease.Poll(now, false, true);
        now += 33.0;
        REQUIRE(aRelease.Poll(now, false, true));
    };

    // Taken by another player's hand: when this game moves it again, it was not thrown here.
    Release taken;
    cThrow(taken);
    taken.Rest();
    CHECK_FALSE(taken.IsThrown());
    for (int i = 0; i < 5; ++i)
    {
        now += 33.0;
        CHECK_FALSE(taken.Poll(now, false, true));
    }

    // Come to rest out of the hand: a later knock is not a throw.
    Release rested;
    cThrow(rested);
    now += 33.0;
    CHECK_FALSE(rested.Poll(now, false, false));
    CHECK_FALSE(rested.IsThrown());
    for (int i = 0; i < 5; ++i)
    {
        now += 33.0;
        CHECK_FALSE(rested.Poll(now, false, true));
    }
}

TEST_CASE("a thrown object is handed to physics once its let-go is shown, and taken back by an update not thrown", "[heldobjects]")
{
    const Flight cFlight{{400.f, 0.f, 200.f}, {0.f, 3.f, 0.f}};
    Launch launch;
    CHECK(launch.Note(100.0, false, {}, false, {}) == Launch::Step::Follow);
    CHECK(launch.Note(133.0, true, cFlight, false, {}) == Launch::Step::Follow);
    CHECK(launch.IsPending());
    // A later thrown update keeps the first one's moment and flight.
    CHECK(launch.Note(166.0, true, {{1.f, 0.f, 0.f}, {}}, false, {}) == Launch::Step::Follow);

    CHECK_FALSE(launch.Due(120.0));
    CHECK_FALSE(launch.IsFlying());
    CHECK(launch.Due(133.0));
    CHECK(launch.IsFlying());
    NearPosition(launch.GetFlight().Velocity, cFlight.Velocity, 0.f);
    NearPosition(launch.GetFlight().Spin, cFlight.Spin, 0.f);
    CHECK_FALSE(launch.Due(200.0));

    // In flight here, its thrown updates are not drawn.
    CHECK(launch.Note(200.0, true, cFlight, false, {}) == Launch::Step::Skip);
    // Its put-down takes it back to be settled where the mover left it.
    CHECK(launch.Note(900.0, false, {}, true, {}) == Launch::Step::TakeBack);
    CHECK_FALSE(launch.IsFlying());
    CHECK(launch.Note(933.0, false, {}, false, {}) == Launch::Step::Follow);
}

TEST_CASE("a let-go caught again before it is shown is never handed to physics", "[heldobjects]")
{
    Launch launch;
    launch.Note(100.0, true, {{100.f, 0.f, 0.f}, {}}, false, {});
    CHECK(launch.IsPending());
    launch.Note(133.0, false, {}, false, {});
    CHECK_FALSE(launch.IsPending());
    CHECK_FALSE(launch.Due(1000.0));

    // A thrown put-down is a put-down.
    Launch atRest;
    CHECK(atRest.Note(100.0, true, {{100.f, 0.f, 0.f}, {}}, true, {}) == Launch::Step::Follow);
    CHECK_FALSE(atRest.IsPending());

    // A flight that is not a number is handed on as none.
    Launch broken;
    const float cNan = std::numeric_limits<float>::quiet_NaN();
    broken.Note(0.0, true, {{cNan, 0.f, 0.f}, {0.f, cNan, 0.f}}, false, {});
    REQUIRE(broken.Due(0.0));
    NearPosition(broken.GetFlight().Velocity, {0.f, 0.f, 0.f}, 0.f);
    NearPosition(broken.GetFlight().Spin, {0.f, 0.f, 0.f}, 0.f);
}

TEST_CASE("a hand here that takes a thrown object in its flight catches it, and keeps it while it holds it", "[heldobjects]")
{
    Launch launch;
    launch.Note(100.0, true, {{300.f, 0.f, 0.f}, {}}, false, {});
    REQUIRE(launch.Due(100.0));
    REQUIRE(launch.IsFlying());

    CatchHold caught;
    // Flying past, out of reach: not caught.
    CHECK_FALSE(caught.Poll(launch.IsFlying(), false));
    CHECK_FALSE(caught.IsCaught());
    // Taken in a hand here: caught once, and kept while held.
    CHECK(caught.Poll(launch.IsFlying(), true));
    CHECK(caught.IsCaught());
    launch = {};
    for (int i = 0; i < 10; ++i)
    {
        CHECK_FALSE(caught.Poll(launch.IsFlying(), true));
        CHECK(caught.IsCaught());
    }
    // Let go: the other players' moves of it count again.
    CHECK_FALSE(caught.Poll(false, false));
    CHECK_FALSE(caught.IsCaught());

    // A hand on an object another player carries, not thrown: no catch, it stays theirs.
    CatchHold carried;
    for (int i = 0; i < 10; ++i)
        CHECK_FALSE(carried.Poll(false, true));
    CHECK_FALSE(carried.IsCaught());
}

TEST_CASE("a flight here that goes on too long or too far is taken back and drawn from the updates", "[heldobjects]")
{
    const Flight cFlight{{300.f, 0.f, 0.f}, {}};
    const glm::vec3 cLetGo{100.f, 200.f, 0.f};

    // Rolling on and on: taken back once it has flown kMaxFlightFor.
    Launch rolling;
    rolling.Note(0.0, true, cFlight, false, cLetGo);
    REQUIRE(rolling.Due(0.0));
    CHECK(rolling.Note(Launch::kMaxFlightFor - 33.0, true, cFlight, false, cLetGo + glm::vec3{900.f, 0.f, 0.f}) == Launch::Step::Skip);
    CHECK(rolling.Note(Launch::kMaxFlightFor + 33.0, true, cFlight, false, cLetGo + glm::vec3{1000.f, 0.f, 0.f}) == Launch::Step::TakeBack);
    CHECK_FALSE(rolling.IsFlying());
    // The rest of that throw is drawn, never handed to physics again.
    for (double t = Launch::kMaxFlightFor + 66.0; t < Launch::kMaxFlightFor + 1000.0; t += 33.0)
    {
        CHECK(rolling.Note(t, true, cFlight, false, cLetGo) == Launch::Step::Follow);
        CHECK_FALSE(rolling.IsPending());
        CHECK_FALSE(rolling.Due(t));
    }
    // Held again and thrown anew: a new flight.
    CHECK(rolling.Note(5000.0, false, {}, false, cLetGo) == Launch::Step::Follow);
    rolling.Note(5033.0, true, cFlight, false, cLetGo);
    CHECK(rolling.Due(5033.0));

    // Falling far, through the world say: taken back past kMaxFlightDistance from the let-go.
    Launch falling;
    falling.Note(0.0, true, cFlight, false, cLetGo);
    REQUIRE(falling.Due(0.0));
    CHECK(falling.Note(500.0, true, cFlight, false, cLetGo - glm::vec3{0.f, 0.f, Launch::kMaxFlightDistance - 10.f}) == Launch::Step::Skip);
    CHECK(falling.Note(533.0, true, cFlight, false, cLetGo - glm::vec3{0.f, 0.f, Launch::kMaxFlightDistance + 10.f}) == Launch::Step::TakeBack);
    CHECK(falling.Note(566.0, true, cFlight, false, cLetGo) == Launch::Step::Follow);
    CHECK_FALSE(falling.IsFlying());
}
