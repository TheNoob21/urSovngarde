#pragma once

#include <Structs/QuestStep.h>

#include <algorithm>
#include <cstdint>

// The quest starts, stages and stops a party was sent, so a player who joins later, or comes back after losing the
// connection, catches up on them. Each stage once since its quest's last start: the joining game takes a stage only
// if it has not reached it yet.
class QuestHistory
{
public:
    // Statuses as in NotifyQuestUpdate.
    static constexpr uint8_t kStageUpdate = 0;
    static constexpr uint8_t kStarted = 1;
    static constexpr uint8_t kStopped = 2;

    // A long session's worth. The oldest go first.
    static constexpr size_t kMax = 2048;

    void Record(const QuestStep& acStep) noexcept
    {
        if (acStep.Status == kStarted)
        {
            // A quest started again runs its stages again, so only this run is caught up on.
            std::erase_if(m_steps, [&acStep](const QuestStep& acOld) { return acOld.Id == acStep.Id; });
        }
        else if (acStep.Status == kStageUpdate)
        {
            const bool cKept = std::any_of(
                m_steps.begin(), m_steps.end(), [&acStep](const QuestStep& acOld) { return acOld.Id == acStep.Id && acOld.Status == kStageUpdate && acOld.Stage == acStep.Stage; });
            if (cKept)
                return;
        }

        if (m_steps.size() >= kMax)
            m_steps.erase(m_steps.begin());
        m_steps.push_back(acStep);
    }

    const TiltedPhoques::Vector<QuestStep>& GetSteps() const noexcept { return m_steps; }

    // Whether a step is progress the party has not had in this run of its quest: a stage not kept since the last start,
    // a start of a quest not running for the party, a stop of one that is (or that the party never saw).
    // A member behind the party reaching a stage it already had is not new (its switches would take the world back).
    // Whether a member's step opens the window in which its game's switches are taken (ObjectService, SharedHomes::SwitchesTaken):
    // never the leader's (always taken), only with the party's steps kept (the switch on, so IsNew can tell a member behind the party),
    // only a step new to the party, and a stage only of a quest already in that player's log (a start or stop puts it there or ends it).
    static constexpr bool OpensSwitchWindow(bool aIsLeader, bool aCatchUpOn, bool aQuestKnownBefore, uint8_t aStatus, bool aNew) noexcept
    {
        return !aIsLeader && aCatchUpOn && aNew && (aStatus != kStageUpdate || aQuestKnownBefore);
    }

    [[nodiscard]] bool IsNew(const QuestStep& acStep) const noexcept
    {
        const auto cLast = std::find_if(m_steps.rbegin(), m_steps.rend(), [&acStep](const QuestStep& acOld) { return acOld.Id == acStep.Id; });
        const bool cSeen = cLast != m_steps.rend();
        if (acStep.Status == kStageUpdate)
            return std::none_of(
                m_steps.begin(), m_steps.end(), [&acStep](const QuestStep& acOld) { return acOld.Id == acStep.Id && acOld.Status == kStageUpdate && acOld.Stage == acStep.Stage; });
        if (acStep.Status == kStarted)
            return !cSeen || cLast->Status == kStopped;
        return !cSeen || cLast->Status != kStopped;
    }

    // A game asking again (RequestQuestCatchUp, after it loaded a save while connected) is answered at most this often.
    // Shorter than any load, so a quick second load (a reload after a death) is still answered.
    static constexpr int64_t kAskAgainAfterMs = 1000;

    // Whether a player joining the party is sent its steps: a member, not the leader of a party it starts, only with the switch on
    // (Gameplay:bEnableQuestCatchUp), never an empty list.
    static constexpr bool MaySendCatchUp(bool aIsLeader, bool aCatchUpOn, bool aHasSteps) noexcept { return !aIsLeader && aCatchUpOn && aHasSteps; }

    // Whether a game that loaded a save while connected is sent the party's steps again (RequestQuestCatchUp): any member, the leader
    // too, as the party's progress wins over an older save (a leader catches up like a member), forward only (QuestCatchUpStep).
    // aMsSinceLastAsk: the time since this player's last answered ask; a negative number means none before.
    static constexpr bool MayAnswerAsk(bool aCatchUpOn, bool aHasSteps, int64_t aMsSinceLastAsk) noexcept
    {
        return aCatchUpOn && aHasSteps && (aMsSinceLastAsk < 0 || aMsSinceLastAsk >= kAskAgainAfterMs);
    }

private:
    TiltedPhoques::Vector<QuestStep> m_steps;
};
