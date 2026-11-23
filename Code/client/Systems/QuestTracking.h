#pragma once

// Which quests a member's journal follows for its party leader: plain values, so the rule is unit-tested.
//
// The journal follows (tracks) the leader's quests, the ones whose markers its compass and map show. Only a quest that
// runs here can be followed; one that does not yet is followed once it does. A member may still pick others itself:
// those stay until the leader changes its own, except when it first follows this leader, which leaves the leader's only.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace QuestTracking
{
struct Changes
{
    std::vector<uint32_t> Track;
    std::vector<uint32_t> Untrack;
    // The leader's quests followed here from now on, for the next call.
    std::vector<uint32_t> Followed;
};

// acLeader: the leader's quests, as this game's form ids. acFollowed: Followed from the last call.
// acTrackedHere: the quests this journal follows that the leader decides on when it is first followed.
// aRunsHere(formId): whether the quest runs here.
template <class TRunsHere>
Changes Follow(const std::vector<uint32_t>& acLeader, const std::vector<uint32_t>& acFollowed, const std::vector<uint32_t>& acTrackedHere, bool aFirst, TRunsHere&& aRunsHere)
{
    const auto cHas = [](const std::vector<uint32_t>& acList, uint32_t aId)
    {
        return std::find(acList.begin(), acList.end(), aId) != acList.end();
    };

    Changes changes;
    for (const uint32_t cId : acLeader)
    {
        if (!aRunsHere(cId) || cHas(changes.Followed, cId))
            continue;

        changes.Followed.push_back(cId);
        if (!cHas(acFollowed, cId))
            changes.Track.push_back(cId);
    }

    // The leader stopped following it.
    for (const uint32_t cId : acFollowed)
    {
        if (!cHas(acLeader, cId))
            changes.Untrack.push_back(cId);
    }

    if (aFirst)
    {
        for (const uint32_t cId : acTrackedHere)
        {
            if (!cHas(acLeader, cId) && !cHas(changes.Untrack, cId))
                changes.Untrack.push_back(cId);
        }
    }

    return changes;
}
} // namespace QuestTracking
