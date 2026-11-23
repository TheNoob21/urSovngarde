#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_set>

//! Which plugins were already reported as unmatched, so a form seen many times is logged once.
//!
//! Forms are looked up from the game's own threads while the mapping can be rebuilt, so recording and clearing share one lock.
//! A lookup's answer never depends on this: it only decides whether a message is written.
class UnmatchedIds
{
public:
    //! True the first time aKey is seen since the last Clear.
    bool First(const uint32_t aKey) noexcept
    {
        std::lock_guard<std::mutex> cLock(m_mutex);
        return m_seen.insert(aKey).second;
    }

    void Clear() noexcept
    {
        std::lock_guard<std::mutex> cLock(m_mutex);
        m_seen.clear();
    }

    //! The key of a form of this game: its plugin slot, and for a light plugin its own index (one slot, 0xFE, holds up to 4096 of them).
    static uint32_t LocalKey(const uint32_t aGameId) noexcept
    {
        const uint32_t cSlot = aGameId >> 24;
        return cSlot == 0xFE ? 0x1000u + ((aGameId >> 12) & 0xFFFu) : cSlot;
    }

private:
    std::mutex m_mutex;
    std::unordered_set<uint32_t> m_seen;
};
