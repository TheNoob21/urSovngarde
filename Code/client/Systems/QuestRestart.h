#pragma once

// Whether a quest stopped here takes a stage the party sends: plain values, so the rule is unit-tested.
//
// Setting a stage on a stopped quest starts it again, from its first stage, and clears the stages it did. A quest that
// ran here and stopped itself, as a misc quest done here before the next stage of the party's run comes, would run
// again and may give its reward again. A run the party starts sends its start first, so its stages are still taken,
// even while the game leaves that start for a later frame.

namespace QuestRestart
{
struct QuestHere
{
    // Neither running nor starting.
    bool Stopped;
    // It reached a stage or completed: a stopped quest that never ran reads stage 0.
    bool Ran;
    // The party's start was applied here, and the quest has not been seen running since.
    bool StartedByParty;
};

[[nodiscard]] constexpr bool TakesStage(const QuestHere& acQuest) noexcept
{
    return !acQuest.Stopped || !acQuest.Ran || acQuest.StartedByParty;
}
} // namespace QuestRestart
