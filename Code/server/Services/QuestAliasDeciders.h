#pragma once

#include <TiltedCore/Buffer.hpp>

#include <Structs/GameId.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>

// Which game's alias fills a party follows for each quest run: plain values, so the rules are unit-tested.
//
// The first player whose start of the quest reaches the server since it last stopped decides. A quest already running
// with no start seen (saves loaded, a party formed later) is decided by the party leader, whose game the others follow.
// The claim holds until the quest stops or the decider is no longer in the party. Stage relays are not affected.
class QuestAliasDeciders
{
public:
    // Quest runs remembered for each party, as many as the alias history keeps. A full party forgets one to make room;
    // the leader then decides that quest, as for any quest with no start seen.
    static constexpr size_t kMaxPerParty = 128;

    void OnStarted(uint32_t aPartyId, const GameId& acQuest, uint32_t aPlayerId)
    {
        auto& quests = m_deciders[aPartyId];
        if (!quests.contains(acQuest))
            Claim(quests, acQuest, aPlayerId);
    }

    void OnStopped(uint32_t aPartyId, const GameId& acQuest)
    {
        auto it = m_deciders.find(aPartyId);
        if (it != m_deciders.end())
            it->second.erase(acQuest);
    }

    // Whether aPlayerId's fills are the ones to relay. With no claim, or one whose player left the party, the leader claims.
    bool Accept(uint32_t aPartyId, const GameId& acQuest, uint32_t aPlayerId, bool aIsLeader, const std::function<bool(uint32_t)>& acIsMember)
    {
        auto& quests = m_deciders[aPartyId];
        auto it = quests.find(acQuest);
        if (it != quests.end() && (it->second == aPlayerId || acIsMember(it->second)))
            return it->second == aPlayerId;
        if (!aIsLeader)
            return false;
        Claim(quests, acQuest, aPlayerId);
        return true;
    }

    // A disbanded party's quest runs are forgotten with it.
    void ForgetParty(uint32_t aPartyId) { m_deciders.erase(aPartyId); }

    void ForgetPlayer(uint32_t aPlayerId)
    {
        for (auto& [_, quests] : m_deciders)
            std::erase_if(quests, [aPlayerId](const auto& acEntry) { return acEntry.second == aPlayerId; });
        std::erase_if(m_deciders, [](const auto& acEntry) { return acEntry.second.empty(); });
    }

    // For tests and logs: the decider of a quest run, if any.
    std::optional<uint32_t> DeciderOf(uint32_t aPartyId, const GameId& acQuest) const
    {
        auto it = m_deciders.find(aPartyId);
        if (it == m_deciders.end())
            return std::nullopt;
        auto cQuest = it->second.find(acQuest);
        if (cQuest == it->second.end())
            return std::nullopt;
        return cQuest->second;
    }

private:
    using Quests = std::unordered_map<GameId, uint32_t>;

    static void Claim(Quests& aQuests, const GameId& acQuest, uint32_t aPlayerId)
    {
        if (!aQuests.contains(acQuest) && aQuests.size() >= kMaxPerParty)
            aQuests.erase(aQuests.begin());
        aQuests[acQuest] = aPlayerId;
    }

    std::unordered_map<uint32_t, Quests> m_deciders;
};
