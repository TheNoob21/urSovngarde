#pragma once

#include <chrono>

//! When a file changed since it was last written is written again. Writing on every change put a burst of rewrites on
//! the server's tick; a change is now on disk at once after a quiet spell, and otherwise once kGap has passed since the
//! last write.
class DeferredSave
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kGap = std::chrono::seconds(5);

    void Mark() noexcept { m_dirty = true; }
    [[nodiscard]] bool IsDirty() const noexcept { return m_dirty; }

    //! True when the caller should write now, which then counts as the last write.
    bool Due(Clock::time_point aNow) noexcept
    {
        if (!m_dirty || (m_written && aNow - m_lastWrite < kGap))
            return false;
        Done(aNow);
        return true;
    }

    //! For a write that cannot wait, when the session ends or the server stops: true whenever there is a change to write.
    bool Flush(Clock::time_point aNow) noexcept
    {
        if (!m_dirty)
            return false;
        Done(aNow);
        return true;
    }

private:
    void Done(Clock::time_point aNow) noexcept
    {
        m_dirty = false;
        m_written = true;
        m_lastWrite = aNow;
    }

    bool m_dirty{};
    bool m_written{};
    Clock::time_point m_lastWrite{};
};
