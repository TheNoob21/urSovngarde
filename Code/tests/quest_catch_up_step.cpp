#include <catch2/catch.hpp>

#include <Systems/QuestCatchUpStep.h>

using namespace QuestCatchUpStep;

namespace
{
Here Running(bool aStageReached = false)
{
    return {true, false, false, aStageReached, false};
}
} // namespace

TEST_CASE("the quest catch-up only moves a member's save forward, never back", "[quests]")
{
    SECTION("a stage the older save skipped is run")
    {
        CHECK(Decide(kStageUpdate, Running(false)) == Take::kApply);
    }

    SECTION("a stage the save already ran is not run again")
    {
        CHECK(Decide(kStageUpdate, Running(true)) == Take::kSkip);
        CHECK(Decide(kStageUpdate, {true, true, true, true, false}) == Take::kSkip);
    }

    SECTION("a quest that runs here is not started again from its first stage, nor stopped by that old run's stop")
    {
        CHECK(Decide(kStarted, Running()) == Take::kSkipStart);
        CHECK(Decide(kStopped, {true, false, false, false, true}) == Take::kSkip);
    }

    SECTION("a quest the save has finished is not started again")
    {
        CHECK(Decide(kStarted, {true, true, true, false, false}) == Take::kSkipStart);
    }

    SECTION("a quest that never ran here is started, as the party did after this save")
    {
        CHECK(Decide(kStarted, {true, true, false, false, false}) == Take::kApply);
    }

    SECTION("the party's stop of a run this save does not have goes through")
    {
        CHECK(Decide(kStopped, Running()) == Take::kApply);
    }
}

TEST_CASE("the leader never holds a caught-up stage behind a playing scene", "[quests]")
{
    CHECK(LeaderHolds(true, kStageUpdate, false));
    CHECK_FALSE(LeaderHolds(true, kStageUpdate, true));
    CHECK_FALSE(LeaderHolds(false, kStageUpdate, false));
    CHECK_FALSE(LeaderHolds(true, kStarted, false));
    CHECK_FALSE(LeaderHolds(true, kStopped, false));
}

TEST_CASE("a leader that catches up neither restarts the party's radiant quests nor sends its older war and journal first", "[quests]")
{
    SECTION("a radiant start is left out when its run ended, or for the leader that does not run it")
    {
        CHECK(LeavesOutRadiantStart(true, false, false));
        CHECK(LeavesOutRadiantStart(false, true, false));
        CHECK_FALSE(LeavesOutRadiantStart(false, true, true));
        CHECK_FALSE(LeavesOutRadiantStart(false, false, false));
    }

    SECTION("the leader's war and tracked quests wait while it catches up or waits for the answer")
    {
        CHECK(HoldsLeaderSends(true, false));
        CHECK(HoldsLeaderSends(false, true));
        CHECK_FALSE(HoldsLeaderSends(false, false));
    }
}

TEST_CASE("the quest catch-up passes over a bounded run of steps in one frame", "[quest_catch_up]")
{
    using namespace QuestCatchUpStep;
    // After the first step passed over, more follow in the same frame...
    CHECK(TakesNextThisFrame(1));
    CHECK(TakesNextThisFrame(kPassedOverPerFrame - 1));
    // ...up to the bound, so a long history of steps this save has reached never stalls one frame for long.
    CHECK_FALSE(TakesNextThisFrame(kPassedOverPerFrame));
    CHECK_FALSE(TakesNextThisFrame(kPassedOverPerFrame + 1));
    // A history of 2048 steps, all reached already, is passed over in 32 frames rather than 2048.
    CHECK((2048 + kPassedOverPerFrame - 1) / kPassedOverPerFrame == 32);
}

TEST_CASE("a radiant quest's caught-up steps wait only for targets another game sends", "[quest_catch_up]")
{
    using namespace QuestCatchUpStep;
    // The game that picked the targets never has another game's: it does not wait for any.
    CHECK_FALSE(WaitsForPartyTargets(false, false, true));
    // A member waits while the picker's targets are not applied here yet...
    CHECK(WaitsForPartyTargets(true, true, false));
    // ...and while none have come yet, so that its stages do not run on its own picks first.
    CHECK(WaitsForPartyTargets(false, false, false));
    // Applied targets are not waited for.
    CHECK_FALSE(WaitsForPartyTargets(true, false, false));
    // Another game's targets win over an offer of this game's own.
    CHECK(WaitsForPartyTargets(true, true, true));
}
