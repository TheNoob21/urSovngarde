#pragma once

#include <chrono>

//! Which frames read the debug keys (F3, F4, F6, F7, F8) on VR.
//! Under Wine every GetAsyncKeyState is a round trip to the Wine server, so reading them on every frame is a cost of its own.
//! They are read once per interval instead, by every service that reads one, in the same frame.
//! DebugService calls Advance once per frame, before any key is read; a service that runs before it in the frame
//! reads the answer of the frame before, so each key is still read once per interval.
struct KeyPollGate
{
    using Clock = std::chrono::steady_clock;

    //! The time between two reads.
    static constexpr std::chrono::milliseconds cInterval{100};

    //! Decides whether this frame reads the keys. The first frame does.
    void Advance(Clock::time_point aNow) noexcept
    {
        // A clock that moved back reads at once, so the gate cannot stay shut.
        m_open = !m_started || aNow < m_lastRead || aNow - m_lastRead >= cInterval;
        if (m_open)
        {
            m_started = true;
            m_lastRead = aNow;
        }
    }

    //! Whether the frame that last called Advance reads the keys.
    [[nodiscard]] bool IsOpen() const noexcept { return m_open; }

    //! The gate the debug key readers share. Main thread only.
    [[nodiscard]] static KeyPollGate& Shared() noexcept
    {
        static KeyPollGate s_gate;
        return s_gate;
    }

private:
    Clock::time_point m_lastRead{};
    bool m_started = false;
    bool m_open = false;
};
