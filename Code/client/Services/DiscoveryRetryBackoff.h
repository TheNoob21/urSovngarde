#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

//! Where the player is, as far as a cell visit is concerned: the world space and the grid cell in an exterior,
//! or the cell in an interior. Two visits are about the same place when these are equal.
struct DiscoveryPlace
{
    uint32_t WorldSpaceId = 0;
    int32_t GridX = 0;
    int32_t GridY = 0;
    uint32_t CellId = 0;

    bool operator==(const DiscoveryPlace& acRhs) const noexcept = default;
};

//! How soon DiscoveryService retries a cell visit that failed (a server mod id it could not find, a cell it could not get).
//! A failed visit leaves the game's grid unrecorded, so without a wait the same scan and the same error lines repeat on every frame.
//! The wait belongs to the place that failed: the player in another place is visited at once, as before. For the same place it starts
//! short, so a failure that heals on its own (a cell still loading) costs a fraction of a second, and doubles with each failure in a
//! row up to a cap. A visit that went through clears it. A forced visit (connecting, a cell announced again) ignores it.
struct DiscoveryRetryBackoff
{
    using Clock = std::chrono::steady_clock;

    //! The wait after the first failure in a place.
    static constexpr std::chrono::milliseconds cFirstInterval{250};
    //! The longest wait, reached after a few failures in a row.
    static constexpr std::chrono::milliseconds cMaxInterval{5000};

    [[nodiscard]] bool MayVisit(Clock::time_point aNow, const DiscoveryPlace& acPlace) const noexcept { return m_streak == 0 || acPlace != m_place || aNow >= m_retryAt; }

    void NoteFailure(Clock::time_point aNow, const DiscoveryPlace& acPlace) noexcept
    {
        // Another place starts over from the short wait.
        if (acPlace != m_place)
            m_streak = 0;

        auto wait = cFirstInterval;
        for (uint32_t i = 0; i < m_streak && wait < cMaxInterval; ++i)
            wait *= 2;

        m_retryAt = aNow + std::min(wait, cMaxInterval);
        m_place = acPlace;
        ++m_streak;
        ++m_total;
    }

    void NoteSuccess() noexcept { m_streak = 0; }

    //! Whether the visits in a row that failed were in this place, so another failure there repeats what was already reported.
    [[nodiscard]] bool IsFailingIn(const DiscoveryPlace& acPlace) const noexcept { return m_streak > 0 && acPlace == m_place; }

    //! How many visits in a row failed in the same place.
    [[nodiscard]] uint32_t Streak() const noexcept { return m_streak; }

    //! How many visits ever failed. Only grows, so a visit can tell whether anything failed while it ran.
    [[nodiscard]] uint32_t TotalFailures() const noexcept { return m_total; }

private:
    DiscoveryPlace m_place{};
    Clock::time_point m_retryAt{};
    uint32_t m_streak = 0;
    uint32_t m_total = 0;
};
