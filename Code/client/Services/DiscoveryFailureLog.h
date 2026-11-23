#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

//! Which of a cell visit's failures are logged as errors.
//! A visit that cannot find the server id of a cell (before the server's mod list arrives, or for a cell of a plugin the server does not
//! have) tries every cell of the 5 by 5 grid, and tries again after a wait. Each try used to log one error line per cell, and every line is
//! flushed to disk and console, which costs a Wine call each. A form id is now an error once per place that fails: the retries in the same
//! place, and a form id met twice in the same visit (the player's own cell is also one of the grid's), are left for the debug level.
struct DiscoveryFailureLog
{
    //! Starts a visit.
    //! @param aRepeatInPlace The visit before this one failed in the same place, and its failures were already logged as errors.
    void BeginVisit(bool aRepeatInPlace) noexcept
    {
        m_repeatInPlace = aRepeatInPlace;
        m_seen.clear();
    }

    //! Whether this failure is logged as an error. False means log it at the debug level.
    [[nodiscard]] bool ShouldLogError(uint32_t aFormId) noexcept
    {
        if (std::find(m_seen.begin(), m_seen.end(), aFormId) != m_seen.end())
            return false;

        m_seen.push_back(aFormId);
        return !m_repeatInPlace;
    }

private:
    std::vector<uint32_t> m_seen;
    bool m_repeatInPlace = false;
};
