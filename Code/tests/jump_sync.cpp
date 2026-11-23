#include <catch2/catch.hpp>

#include <Systems/JumpSync.h>

// Another player's jump plays the game's jump animations on their copy.

using namespace JumpSync;
using namespace std::chrono_literals;

TEST_CASE("Jump sync: the jump, fall and landing events", "[jump_sync]")
{
    CHECK(IsJumpEvent("JumpStandingStart"));
    CHECK(IsJumpEvent("JumpDirectionalStart"));
    CHECK(IsJumpEvent("JumpFall"));
    CHECK(IsJumpEvent("JumpFallDirectional"));
    CHECK(IsJumpEvent("JumpLand"));
    CHECK(IsJumpEvent("JumpLandDirectional"));
    CHECK(IsJumpEvent("jumpLandSoft"));
    CHECK_FALSE(IsJumpEvent("Jump"));
    CHECK_FALSE(IsJumpEvent(""));
    CHECK_FALSE(IsJumpEvent("moveStart"));
    CHECK_FALSE(IsJumpEvent("Ragdoll"));
    CHECK_FALSE(IsJumpEvent("attackStartJump"));
}

TEST_CASE("Jump sync: which refused actions go to the copy's graph", "[jump_sync]")
{
    CHECK(SendsRefusedEvent(true, false, "JumpStandingStart", false));
    CHECK(SendsRefusedEvent(true, false, "JumpLand", false));
    // Taken already, an NPC copy, another action, or a copy dead or knocked down here: nothing more is sent.
    CHECK_FALSE(SendsRefusedEvent(true, true, "JumpStandingStart", false));
    CHECK_FALSE(SendsRefusedEvent(false, false, "JumpStandingStart", false));
    CHECK_FALSE(SendsRefusedEvent(true, false, "moveStart", false));
    CHECK_FALSE(SendsRefusedEvent(true, false, "JumpFall", true));
}

TEST_CASE("Jump sync: a VR player's legs leave the pose in the air and come back after landing", "[jump_sync]")
{
    const auto cNow = Clock::now();

    SECTION("on the ground they are sent")
    {
        Legs legs;
        CHECK(legs.Sent(false, cNow));
        CHECK(legs.Sent(false, cNow + 1s));
    }
    SECTION("a jump: out in the air, out for the landing, then back")
    {
        Legs legs;
        CHECK(legs.Sent(false, cNow));
        CHECK_FALSE(legs.Sent(true, cNow + 10ms));
        CHECK_FALSE(legs.Sent(true, cNow + 600ms));
        CHECK_FALSE(legs.Sent(false, cNow + 700ms));
        CHECK_FALSE(legs.Sent(false, cNow + 700ms + kLandHold - 1ms));
        CHECK(legs.Sent(false, cNow + 700ms + kLandHold));
    }
    SECTION("a second jump during the landing starts over")
    {
        Legs legs;
        CHECK_FALSE(legs.Sent(true, cNow));
        CHECK_FALSE(legs.Sent(false, cNow + 100ms));
        CHECK_FALSE(legs.Sent(true, cNow + 200ms));
        CHECK_FALSE(legs.Sent(false, cNow + 300ms));
        CHECK_FALSE(legs.Sent(false, cNow + 300ms + kLandHold - 1ms));
        CHECK(legs.Sent(false, cNow + 300ms + kLandHold));
    }
}

TEST_CASE("Jump sync: where a jump event leaves the graph", "[jump_sync]")
{
    CHECK(PhaseOf("JumpStandingStart") == Phase::Up);
    CHECK(PhaseOf("JumpDirectionalStart") == Phase::Up);
    CHECK(PhaseOf("JumpFall") == Phase::Falling);
    CHECK(PhaseOf("jumpFallDirectional") == Phase::Falling);
    CHECK(PhaseOf("JumpLand") == Phase::Ground);
    CHECK(PhaseOf("JUMPLANDDIRECTIONAL") == Phase::Ground);
    CHECK(PhaseOf("JumpLandSoft") == Phase::Ground);
}

TEST_CASE("Jump sync: a copy still in its jump is landed once its owner is on the ground", "[jump_sync]")
{
    const auto cNow = Clock::now();

    SECTION("on the ground nothing is sent")
    {
        Landing landing;
        CHECK(landing.Update(false, cNow) == Synth::None);
        CHECK(landing.Update(false, cNow + 5s) == Synth::None);
    }
    SECTION("a whole jump played on the copy, its landing too: nothing is sent")
    {
        Landing landing;
        landing.OnCopyEvent("JumpStandingStart", true, cNow);
        CHECK(landing.Update(true, cNow + 50ms) == Synth::None);
        landing.OnCopyEvent("JumpFall", true, cNow + 400ms);
        CHECK(landing.Update(true, cNow + 500ms) == Synth::None);
        landing.OnCopyEvent("JumpLand", true, cNow + 800ms);
        CHECK(landing.Update(false, cNow + 820ms) == Synth::None);
        CHECK(landing.Update(false, cNow + 2s) == Synth::None);
    }
    SECTION("its owner's landing never came: the copy is landed after the grace")
    {
        Landing landing;
        landing.OnCopyEvent("JumpStandingStart", true, cNow);
        CHECK(landing.Update(true, cNow + 50ms) == Synth::None);
        landing.OnCopyEvent("JumpFall", true, cNow + 400ms);
        CHECK(landing.Update(false, cNow + 800ms) == Synth::None);
        CHECK(landing.Update(false, cNow + 800ms + kLandGrace - 1ms) == Synth::None);
        CHECK(landing.Update(false, cNow + 800ms + kLandGrace) == Synth::Land);
        CHECK(landing.GetPhase() == Phase::Ground);
        CHECK(landing.Update(false, cNow + 2s) == Synth::None);
    }
    SECTION("a lost fall: the fall first, then the landing on the next frame")
    {
        Landing landing;
        landing.OnCopyEvent("JumpDirectionalStart", true, cNow);
        CHECK(landing.Update(true, cNow + 50ms) == Synth::None);
        CHECK(landing.Update(false, cNow + 800ms) == Synth::None);
        CHECK(landing.Update(false, cNow + 800ms + kLandGrace) == Synth::Fall);
        CHECK(landing.Update(false, cNow + 800ms + kLandGrace + 16ms) == Synth::Land);
        CHECK(landing.Update(false, cNow + 3s) == Synth::None);
    }
    SECTION("its owner back in the air during the grace waits again")
    {
        Landing landing;
        landing.OnCopyEvent("JumpFall", true, cNow);
        CHECK(landing.Update(true, cNow + 10ms) == Synth::None);
        CHECK(landing.Update(false, cNow + 100ms) == Synth::None);
        CHECK(landing.Update(true, cNow + 200ms) == Synth::None);
        CHECK(landing.Update(false, cNow + 300ms) == Synth::None);
        CHECK(landing.Update(false, cNow + 300ms + kLandGrace - 1ms) == Synth::None);
        CHECK(landing.Update(false, cNow + 300ms + kLandGrace) == Synth::Land);
    }
    SECTION("a long fall waits for its owner as long as it reads in the air")
    {
        Landing landing;
        landing.OnCopyEvent("JumpFall", true, cNow);
        CHECK(landing.Update(true, cNow + 10ms) == Synth::None);
        CHECK(landing.Update(true, cNow + 8s) == Synth::None);
    }
    SECTION("an owner never read in the air (a short hop between updates) lands its copy late")
    {
        Landing landing;
        landing.OnCopyEvent("JumpStandingStart", true, cNow);
        CHECK(landing.Update(false, cNow + 10ms) == Synth::None);
        CHECK(landing.Update(false, cNow + kNoAirLand - 1ms) == Synth::None);
        CHECK(landing.Update(false, cNow + kNoAirLand) == Synth::Fall);
        CHECK(landing.Update(false, cNow + kNoAirLand + 16ms) == Synth::Land);
    }
    SECTION("refused events and other events change nothing")
    {
        Landing landing;
        landing.OnCopyEvent("JumpStandingStart", false, cNow);
        landing.OnCopyEvent("moveStart", true, cNow);
        CHECK(landing.GetPhase() == Phase::Ground);
        CHECK(landing.Update(false, cNow + 5s) == Synth::None);
    }
    SECTION("the copy's own landing ends it")
    {
        Landing landing;
        landing.OnCopyEvent("JumpFall", true, cNow);
        landing.OnCopyEvent("JumpLandDirectional", true, cNow + 300ms);
        CHECK(landing.Update(false, cNow + 5s) == Synth::None);
    }
    SECTION("a reset forgets the jump")
    {
        Landing landing;
        landing.OnCopyEvent("JumpFall", true, cNow);
        landing.Reset();
        CHECK(landing.Update(false, cNow + 5s) == Synth::None);
    }
}
