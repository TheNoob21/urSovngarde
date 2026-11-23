#pragma once

#include <Structs/GameId.h>

#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <utility>

//! The references a party leader's scripts switched on or off, the latest state of each, for a member who joins later.
//! Kept to a cap, the longest unchanged going first: unbounded, every joiner's catch-up grew with the session's length.
class ReferenceSwitches
{
public:
    static constexpr size_t kMax = 16384;

    ReferenceSwitches() = default;
    explicit ReferenceSwitches(const size_t aMax) noexcept
        : m_max(aMax)
    {
    }

    void Set(const GameId& acId, const bool aEnabled) noexcept
    {
        const uint64_t cOrder = ++m_lastOrder;
        m_states[acId] = State{aEnabled, cOrder};
        m_order.emplace_back(acId, cOrder);

        while (m_states.size() > m_max && !m_order.empty())
        {
            const auto [cId, cQueued] = m_order.front();
            m_order.pop_front();
            // A reference switched again since has a later place in the queue.
            if (const auto it = m_states.find(cId); it != m_states.end() && it->second.Order == cQueued)
                m_states.erase(it);
        }

        // Places left behind by references switched again are dropped once they outnumber the references.
        if (m_order.size() > 2 * m_max)
        {
            std::deque<std::pair<GameId, uint64_t>> kept;
            for (const auto& cQueued : m_order)
                if (const auto it = m_states.find(cQueued.first); it != m_states.end() && it->second.Order == cQueued.second)
                    kept.push_back(cQueued);
            m_order = std::move(kept);
        }
    }

    [[nodiscard]] std::optional<bool> Find(const GameId& acId) const noexcept
    {
        const auto it = m_states.find(acId);
        if (it == m_states.end())
            return std::nullopt;
        return it->second.Enabled;
    }

    //! Calls aFunctor(id, enabled) for each, oldest change first.
    template <class T> void ForEach(const T& acFunctor) const
    {
        for (const auto& [cId, cQueued] : m_order)
            if (const auto it = m_states.find(cId); it != m_states.end() && it->second.Order == cQueued)
                acFunctor(cId, it->second.Enabled);
    }

    [[nodiscard]] size_t Size() const noexcept { return m_states.size(); }
    [[nodiscard]] bool Empty() const noexcept { return m_states.empty(); }

private:
    struct State
    {
        bool Enabled{};
        uint64_t Order{};
    };

    size_t m_max{kMax};
    uint64_t m_lastOrder{0};
    std::unordered_map<GameId, State> m_states;
    std::deque<std::pair<GameId, uint64_t>> m_order;
};
