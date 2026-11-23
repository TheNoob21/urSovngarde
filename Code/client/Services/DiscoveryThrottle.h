#pragma once

#include <chrono>

//! Which frames the discovery scan (DiscoveryService::OnUpdate) runs on.
//! Connected, every frame. Not connected nobody is told about cells or actors, so it runs a few times a second,
//! and not at all at the main menu or on a loading screen once it holds no actors from the last game.
struct DiscoveryThrottle
{
    using Clock = std::chrono::steady_clock;

    //! How often the scan runs while not connected.
    static constexpr std::chrono::milliseconds cOfflineInterval{250};

    //! @param aOnline The server accepted this player.
    //! @param aCellLoaded The player stands in a cell (false at the main menu and on loading screens).
    //! @param aHasTracked Discovery still holds actors from the last game (after Quit to Main Menu): their removal keeps
    //!        running at the same pace until none are left, so a later connect finds none of them.
    [[nodiscard]] bool ShouldRun(bool aOnline, bool aCellLoaded, bool aHasTracked, Clock::time_point aNow) noexcept
    {
        if (aOnline)
            return true;

        if (m_hasRun && aNow - m_lastCheck < cOfflineInterval)
            return false;

        m_hasRun = true;
        m_lastCheck = aNow;
        return aCellLoaded || aHasTracked;
    }

private:
    Clock::time_point m_lastCheck{};
    bool m_hasRun = false;
};
