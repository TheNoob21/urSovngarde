#pragma once

#include <chrono>
#include <cstdint>
#include <algorithm>
#include <map>
#include <optional>

//! Who moves a loose object now. Only one player's moves are taken at a time: two players pushing the same cart
//! each pulled it back to where their own game had it. The other player's game holds the object while those moves
//! come in and lets it go two seconds after the last, or half a second after one that put it down (the client's
//! DroppedItemService), so the server waits as long.
class ObjectMover
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kHeldFor = std::chrono::seconds(2);
    static constexpr auto kHeldForAtRest = std::chrono::milliseconds(500);

    // A move from anyone else is dropped until the current mover has been quiet for as long as the other games hold it.
    // So is one of the mover's own sent before its last (aTick, its game's tick): moves travel unreliable, and a late one
    // must not undo a put-down.
    bool Take(uint32_t aPlayerId, bool aAtRest, Clock::time_point aNow, uint64_t aTick = 0) noexcept
    {
        if (!Accepts(aPlayerId, aNow, aTick))
            return false;
        m_playerId = aPlayerId;
        m_lastMove = aNow;
        m_heldFor = aAtRest ? Clock::duration(kHeldForAtRest) : Clock::duration(kHeldFor);
        m_tick = aTick;
        return true;
    }

    //! Whether Take would take this move, changing nothing.
    [[nodiscard]] bool Accepts(uint32_t aPlayerId, Clock::time_point aNow, uint64_t aTick = 0) const noexcept
    {
        if (m_playerId && *m_playerId != aPlayerId && aNow - m_lastMove < m_heldFor)
            return false;
        return !(m_playerId && *m_playerId == aPlayerId && aTick && aTick < m_tick);
    }

    std::optional<uint32_t> GetPlayerId() const noexcept { return m_playerId; }

    //! Nobody moves it now: its last mover has been quiet for as long as the other games hold it.
    bool IsLapsed(Clock::time_point aNow) const noexcept { return !m_playerId || aNow - m_lastMove >= m_heldFor; }

private:
    std::optional<uint32_t> m_playerId;
    Clock::time_point m_lastMove{};
    Clock::duration m_heldFor{};
    uint64_t m_tick{};
};

// Keys clients name come and go: a map of movers keeps only those still moving once it has grown past aKeep.
template <class TMovers> void ForgetLapsedMovers(TMovers& aMovers, ObjectMover::Clock::time_point aNow, size_t aKeep)
{
    if (aMovers.size() > aKeep)
        std::erase_if(aMovers, [aNow](const auto& acEntry) { return acEntry.second.IsLapsed(aNow); });
}

// Takes a player's move of the object at acKey. Only a move its mover would take counts against the player's moves per
// second (aRate, with Allow(now)); a put-down always fits. Moves dropped anyway used to spend what the player's real moves needed.
template <class TMovers, class TRate>
bool TakeCapped(TMovers& aMovers, const typename TMovers::key_type& acKey, TRate& aRate, uint32_t aPlayerId, bool aAtRest, ObjectMover::Clock::time_point aNow, uint64_t aTick)
{
    if (const auto cFound = aMovers.find(acKey); cFound != aMovers.end() && !cFound->second.Accepts(aPlayerId, aNow, aTick))
        return false;
    if (!aAtRest && !aRate.Allow(aNow))
        return false;
    return aMovers[acKey].Take(aPlayerId, aAtRest, aNow, aTick);
}

// Makes room for one more entry in a map of at most aMax: the one aAge says is oldest goes, unless acKeep is already there.
template <class TMap, class TAge> void MakeRoom(TMap& aMap, size_t aMax, const typename TMap::key_type& acKeep, TAge&& aAge)
{
    if (aMap.size() < aMax || aMap.contains(acKeep))
        return;
    const auto cOldest = std::min_element(aMap.begin(), aMap.end(), [&aAge](const auto& acLeft, const auto& acRight) { return aAge(acLeft.second) < aAge(acRight.second); });
    if (cOldest != aMap.end())
        aMap.erase(cOldest);
}
