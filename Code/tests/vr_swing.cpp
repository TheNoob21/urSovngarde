#include <catch2/catch.hpp>

#include <VRSwing.h>

using VRSwing::Kind;

TEST_CASE("A VR swing starts when the attack state becomes swing or bash", "[vr_swing]")
{
    REQUIRE(VRSwing::Starts(0, VRSwing::kStateSwing));
    REQUIRE(VRSwing::Starts(0, VRSwing::kStateBash));
    // The next swing out of a follow-through (5) or a hit (3) is a new swing.
    REQUIRE(VRSwing::Starts(5, VRSwing::kStateSwing));
    REQUIRE(VRSwing::Starts(3, VRSwing::kStateSwing));
    REQUIRE(VRSwing::Starts(VRSwing::kStateSwing, VRSwing::kStateBash));
    REQUIRE_FALSE(VRSwing::Starts(VRSwing::kStateSwing, VRSwing::kStateSwing));
    REQUIRE_FALSE(VRSwing::Starts(VRSwing::kStateBash, VRSwing::kStateBash));
    REQUIRE_FALSE(VRSwing::Starts(0, 0));
    // Drawing (1) and the bow's states (8-12) are no swing.
    for (uint32_t state : {1u, 3u, 4u, 5u, 8u, 9u, 10u, 11u, 12u})
        REQUIRE_FALSE(VRSwing::Starts(0, state));
}

TEST_CASE("A VR swing is classified from the attack the game picked", "[vr_swing]")
{
    REQUIRE(VRSwing::Classify(0, true, 0, "attackStart") == Kind::None);
    REQUIRE(VRSwing::Classify(VRSwing::kStateSwing, false, 0, "") == Kind::Right);
    REQUIRE(VRSwing::Classify(VRSwing::kStateBash, false, 0, "") == Kind::Bash);
    REQUIRE(VRSwing::Classify(VRSwing::kStateSwing, true, 0, "attackStart") == Kind::Right);
    REQUIRE(VRSwing::Classify(VRSwing::kStateSwing, true, 0, "attackStartLeftHand") == Kind::Left);
    REQUIRE(VRSwing::Classify(VRSwing::kStateSwing, true, 0, "attackStartDualWield") == Kind::Dual);
    REQUIRE(VRSwing::Classify(VRSwing::kStateSwing, true, VRSwing::kFlagPower, "attackPowerStartForward") == Kind::RightPower);
    REQUIRE(VRSwing::Classify(VRSwing::kStateSwing, true, VRSwing::kFlagPower, "AttackPowerStartLeftHand") == Kind::LeftPower);
    REQUIRE(VRSwing::Classify(VRSwing::kStateSwing, true, VRSwing::kFlagPower, "attackPowerStartDualWield") == Kind::DualPower);
    REQUIRE(VRSwing::Classify(VRSwing::kStateBash, true, VRSwing::kFlagBash, "bashStart") == Kind::Bash);
    REQUIRE(VRSwing::Classify(VRSwing::kStateBash, true, VRSwing::kFlagBash | VRSwing::kFlagPower, "bashPowerStart") == Kind::PowerBash);
    // A bash attack picked while the state reads swing is still a bash.
    REQUIRE(VRSwing::Classify(VRSwing::kStateSwing, true, VRSwing::kFlagBash, "bashStart") == Kind::Bash);
    REQUIRE(VRSwing::Classify(VRSwing::kStateSwing, true, 0, "bashStart") == Kind::Bash);
    // A sidestep power attack to the left is the right hand's.
    REQUIRE(VRSwing::Classify(VRSwing::kStateSwing, true, VRSwing::kFlagPower, "attackPowerStartLeft") == Kind::RightPower);
    // Without data the power flag means nothing.
    REQUIRE(VRSwing::Classify(VRSwing::kStateSwing, false, VRSwing::kFlagPower, "") == Kind::Right);
}

TEST_CASE("Each VR attack becomes the actions a flat player's game performs", "[vr_swing]")
{
    auto steps = VRSwing::StepsFor(Kind::Right, false);
    REQUIRE(steps.Count == 1);
    REQUIRE(steps.Items[0].ActionId == VRSwing::kActionRightAttack);
    REQUIRE(steps.Items[0].EventName == "attackStart");
    REQUIRE_FALSE(steps.RaisedBlock);

    steps = VRSwing::StepsFor(Kind::RightPower, false, "attackPowerStartForward");
    REQUIRE(steps.Count == 1);
    REQUIRE(steps.Items[0].ActionId == VRSwing::kActionRightPowerAttack);
    REQUIRE(steps.Items[0].EventName == "attackPowerStartForward");

    REQUIRE(VRSwing::StepsFor(Kind::Left, false).Items[0].ActionId == VRSwing::kActionLeftAttack);
    REQUIRE(VRSwing::StepsFor(Kind::LeftPower, false).Items[0].ActionId == VRSwing::kActionLeftPowerAttack);
    REQUIRE(VRSwing::StepsFor(Kind::Dual, false).Items[0].ActionId == VRSwing::kActionDualAttack);
    REQUIRE(VRSwing::StepsFor(Kind::DualPower, false).Items[0].ActionId == VRSwing::kActionDualPowerAttack);
    REQUIRE(VRSwing::StepsFor(Kind::None, false).Count == 0);

    // A bash is a right attack while blocking: the block is raised first unless it already is, and lowered later.
    steps = VRSwing::StepsFor(Kind::Bash, false);
    REQUIRE(steps.Count == 2);
    REQUIRE(steps.Items[0].ActionId == VRSwing::kActionLeftAttack);
    REQUIRE(steps.Items[1].ActionId == VRSwing::kActionRightAttack);
    REQUIRE(steps.Items[1].EventName == "bashStart");
    REQUIRE(steps.RaisedBlock);
    steps = VRSwing::StepsFor(Kind::PowerBash, true);
    REQUIRE(steps.Count == 1);
    REQUIRE(steps.Items[0].ActionId == VRSwing::kActionRightPowerAttack);
    REQUIRE(steps.Items[0].EventName == "bashPowerStart");
    REQUIRE_FALSE(steps.RaisedBlock);
    REQUIRE(VRSwing::kReleaseStep.ActionId == VRSwing::kActionLeftRelease);
}

TEST_CASE("A VR swing the game already sent an attack for is not sent again, unless it is more", "[vr_swing]")
{
    REQUIRE(VRSwing::NeedsSending(Kind::Right, -1));
    REQUIRE_FALSE(VRSwing::NeedsSending(Kind::Right, 0));
    REQUIRE(VRSwing::NeedsSending(Kind::Right, 1));
    REQUIRE_FALSE(VRSwing::NeedsSending(Kind::Left, 1));
    REQUIRE(VRSwing::NeedsSending(Kind::RightPower, 0));
    REQUIRE(VRSwing::NeedsSending(Kind::Bash, 0));
    REQUIRE_FALSE(VRSwing::NeedsSending(Kind::None, -1));

    REQUIRE(VRSwing::HandOf(VRSwing::kActionRightPowerAttack) == 0);
    REQUIRE(VRSwing::HandOf(VRSwing::kActionLeftAttack) == 1);
    REQUIRE(VRSwing::HandOf(VRSwing::kActionDualAttack) == 2);
    REQUIRE(VRSwing::HandOf(VRSwing::kActionLeftRelease) == -1);
    REQUIRE(VRSwing::HandOf(0x13451u + 1) == -1);
}

TEST_CASE("A VR swing or bash step from before a load or outside the session is dropped", "[vr_swing]")
{
    REQUIRE(VRSwing::KeepsPending(true, 3, 3));
    // A loading screen queued since, even one gone again before the next update.
    REQUIRE_FALSE(VRSwing::KeepsPending(true, 3, 4));
    // Offline, or a loading screen up now.
    REQUIRE_FALSE(VRSwing::KeepsPending(false, 3, 3));

    // A bash whose second step waits for the next update; a load comes and goes before it.
    uint32_t seen = 7;
    bool hasNextStep = true;
    const auto cUpdate = [&](const bool aInSession, const uint32_t aLoads)
    {
        if (!VRSwing::KeepsPending(aInSession, seen, aLoads))
        {
            hasNextStep = false;
            seen = aLoads;
            return false;
        }
        const bool cSent = hasNextStep;
        hasNextStep = false;
        return cSent;
    };
    REQUIRE_FALSE(cUpdate(true, 8));
    REQUIRE_FALSE(hasNextStep);
    // The next swing after the load is the session's again.
    hasNextStep = true;
    REQUIRE(cUpdate(true, 8));
}

TEST_CASE("A block held up is raised and lowered once, and a bash's own block is left to the bash", "[vr_swing]")
{
    using VRSwing::BlockChange;
    CHECK(VRSwing::HeldBlock(true, false, false) == BlockChange::Raise);
    CHECK(VRSwing::HeldBlock(true, true, false) == BlockChange::None);
    CHECK(VRSwing::HeldBlock(false, true, false) == BlockChange::Lower);
    CHECK(VRSwing::HeldBlock(false, false, false) == BlockChange::None);
    // A block raised for a bash is up: not raised again; once it comes down, a block still held is raised.
    CHECK(VRSwing::HeldBlock(true, false, true) == BlockChange::None);
    CHECK(VRSwing::HeldBlock(false, false, true) == BlockChange::None);
    CHECK(VRSwing::kRaiseStep.ActionId == VRSwing::kActionLeftAttack);
    CHECK(VRSwing::kRaiseStep.EventName == "blockStart");
    CHECK(VRSwing::kReleaseStep.EventName == "blockStop");
}

TEST_CASE("A melee hit with no swing marked is shown as the swing it was", "[vr_swing]")
{
    CHECK(VRSwing::FromHit(0, 0) == Kind::Right);
    CHECK(VRSwing::FromHit(1, 0) == Kind::Left);
    CHECK(VRSwing::FromHit(0, VRSwing::kHitPower) == Kind::RightPower);
    CHECK(VRSwing::FromHit(1, VRSwing::kHitPower) == Kind::LeftPower);
    CHECK(VRSwing::FromHit(1, VRSwing::kHitBash) == Kind::Bash);
    CHECK(VRSwing::FromHit(0, VRSwing::kHitBash | VRSwing::kHitPower) == Kind::PowerBash);
    // Sneak and blocked bits change nothing.
    CHECK(VRSwing::FromHit(0, 2 | 8) == Kind::Right);
}

TEST_CASE("A hit shows no second swing for an attack already shown for its hand", "[vr_swing]")
{
    std::array<uint64_t, 3> shown{};
    CHECK(VRSwing::HitNeedsSwing(Kind::Right, shown, 5000));
    // The game marked the swing and it was sent 200 ms before the hit.
    shown[0] = 4800;
    CHECK_FALSE(VRSwing::HitNeedsSwing(Kind::Right, shown, 5000));
    CHECK(VRSwing::HitNeedsSwing(Kind::Left, shown, 5000));
    // Long enough after, a new hit is a new swing.
    CHECK(VRSwing::HitNeedsSwing(Kind::Right, shown, 4800 + VRSwing::kHitWindow));
    // A dual attack covers either hand.
    shown = {0, 0, 4900};
    CHECK_FALSE(VRSwing::HitNeedsSwing(Kind::Left, shown, 5000));
    // A clock that stepped back counts as nothing shown.
    shown = {6000, 0, 0};
    CHECK(VRSwing::HitNeedsSwing(Kind::Right, shown, 5000));
    CHECK_FALSE(VRSwing::HitNeedsSwing(Kind::None, {}, 5000));
}

TEST_CASE("A bow's or crossbow's step is a change into, out of or within the bow states", "[vr_swing]")
{
    REQUIRE(VRSwing::IsBowStep(0, 8));
    REQUIRE(VRSwing::IsBowStep(10, 11));
    REQUIRE(VRSwing::IsBowStep(15, 0));
    REQUIRE_FALSE(VRSwing::IsBowStep(10, 10));
    // A swing or a bash is no bow step.
    REQUIRE_FALSE(VRSwing::IsBowStep(0, VRSwing::kStateSwing));
    REQUIRE_FALSE(VRSwing::IsBowStep(VRSwing::kStateSwing, VRSwing::kStateBash));
    REQUIRE_FALSE(VRSwing::IsBowStep(7, 16));
}
