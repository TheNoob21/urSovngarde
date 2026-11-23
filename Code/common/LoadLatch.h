#pragma once

#include <cstdint>

// "A save is loading" for the party words. A load the game accepted keeps the latch until its load event, or until the game
// refuses that very request. No menu sight and no passing time ends it: a load can be slow, and the loading menu can open or
// close between two looks while the old player is still readable.
class LoadLatch
{
public:
    void Begin() noexcept
    {
        ++m_generation;
        m_loading = true;
    }

    // The save replaced the player: the load event.
    void Complete() noexcept { m_loading = false; }

    // The native call refused this request. A newer load keeps its own latch.
    void Refused(uint64_t aGeneration) noexcept
    {
        if (m_generation == aGeneration)
            m_loading = false;
    }

    [[nodiscard]] bool Loading() const noexcept { return m_loading; }
    [[nodiscard]] uint64_t Generation() const noexcept { return m_generation; }

private:
    uint64_t m_generation{};
    bool m_loading{};
};
