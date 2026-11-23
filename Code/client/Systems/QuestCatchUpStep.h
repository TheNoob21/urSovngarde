#pragma once

#include <cstddef>
#include <cstdint>

// Whether one kept step of the party's quest catch-up is applied in this game (QuestService::RunCatchUp).
// The catch-up runs on joining, on coming back, and after this game loads a save while connected (RequestQuestCatchUp).
// It only moves a quest forward: it runs the stages this save skipped, and never takes back what the save already has.
namespace QuestCatchUpStep
{
// Statuses as in NotifyQuestUpdate.
constexpr uint8_t kStageUpdate = 0;
constexpr uint8_t kStarted = 1;
constexpr uint8_t kStopped = 2;

// The quest as this game has it now.
struct Here
{
    bool Exists{};       // the quest is in this game's data
    bool Stopped{};      // not running here
    bool Completed{};    // finished here
    bool StageReached{}; // the step's stage already ran here
    bool StartSkipped{}; // this catch-up left the quest's start out (its run here is this save's own)
};

enum class Take
{
    kApply,     // set the step here
    kSkipStart, // the quest runs or is done here: its start and stop are left out, its unreached stages still come
    kSkip,      // nothing to do here
};

constexpr Take Decide(uint8_t aStatus, const Here& acHere) noexcept
{
    // A start would restart a run the save has, from its first stage.
    if (aStatus == kStarted && acHere.Exists && (!acHere.Stopped || acHere.Completed))
        return Take::kSkipStart;
    // So would the stop of that run end the save's own.
    if (aStatus == kStopped && acHere.StartSkipped)
        return Take::kSkip;
    // A stage the save has already run is not run again.
    if (aStatus == kStageUpdate && acHere.StageReached)
        return Take::kSkip;
    return Take::kApply;
}

//! Whether a radiant quest's start in the catch-up is left out here: its run ended in the party (its targets are gone), or this game is the
//! leader and does not run it now. The leader picks the party's targets, and a start from the catch-up offers none, so it would run on new
//! picks while the members keep the old ones; the members' games still run that quest.
constexpr bool LeavesOutRadiantStart(bool aRunEnded, bool aIsLeader, bool aRunsHere) noexcept
{
    return aRunEnded || (aIsLeader && !aRunsHere);
}

//! How many steps the catch-up may pass over in one frame: steps that change nothing here (reached already, left out, or the
//! start of a run this save has finished). A step that is applied, or that waits for a start, still ends the frame.
inline constexpr std::size_t kPassedOverPerFrame = 64;

//! Whether the catch-up takes another step this frame, after aPassedOver steps in a row were passed over in it.
constexpr bool TakesNextThisFrame(std::size_t aPassedOver) noexcept
{
    return aPassedOver < kPassedOverPerFrame;
}

//! Whether a radiant quest's caught-up steps wait for the party's targets (QuestService::IsCatchUpStartDone). Another game's
//! targets that have not been applied here yet are waited for, and so are targets that have not come at all, unless this game
//! offered its own: it picked that run's targets, and nobody else sends any to it.
constexpr bool WaitsForPartyTargets(bool aHasWanted, bool aWantedPending, bool aOfferedHere) noexcept
{
    if (aHasWanted)
        return aWantedPending;
    return !aOfferedHere;
}

//! Whether the leader holds back its war and tracked quests (sent for the members to follow): while its catch-up runs, or while it waits
//! for the answer to its ask after a load. Its older save's war and journal would otherwise reach the members first.
constexpr bool HoldsLeaderSends(bool aCatchUpQueued, bool aAwaitingAnswer) noexcept
{
    return aCatchUpQueued || aAwaitingAnswer;
}

//! Whether the leader may hold a stage for a playing scene that checks it (QuestService::HoldStage): a member's live stage only,
//! never a caught-up one, which the party passed long ago and which would otherwise wait behind a scene this game replays.
constexpr bool LeaderHolds(bool aIsLeader, uint8_t aStatus, bool aCatchingUp) noexcept
{
    return aIsLeader && aStatus == kStageUpdate && !aCatchingUp;
}
} // namespace QuestCatchUpStep
