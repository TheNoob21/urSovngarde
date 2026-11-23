#pragma once

#include <Structs/QuestAliases.h>

#include <unordered_map>
#include <algorithm>
#include <deque>

class QuestAliasHistory
{
public:
    static constexpr size_t kMaxQuests = 128;

    void Start(const GameId& acQuest) { std::erase(m_stopped, acQuest); }

    bool IsStopped(const GameId& acQuest) const noexcept { return std::find(m_stopped.begin(), m_stopped.end(), acQuest) != m_stopped.end(); }

    void Remember(const QuestAliases& acAliases)
    {
        if (IsStopped(acAliases.Quest))
            return;
        auto it = m_quests.find(acAliases.Quest);
        if (it != m_quests.end())
            it->second = acAliases;
        else if (m_quests.size() < kMaxQuests)
            m_quests.emplace(acAliases.Quest, acAliases);
    }

    void Stop(const GameId& acQuest)
    {
        m_quests.erase(acQuest);
        // Keep the most recent stops, including runs whose first fill has not arrived yet.
        std::erase(m_stopped, acQuest);
        if (m_stopped.size() == kMaxQuests)
            m_stopped.pop_front();
        m_stopped.push_back(acQuest);
    }

    const auto& GetQuests() const noexcept { return m_quests; }

private:
    std::unordered_map<GameId, QuestAliases> m_quests;
    std::deque<GameId> m_stopped;
};
