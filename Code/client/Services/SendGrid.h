#pragma once

#include <algorithm>
#include <cassert>
#include <chrono>

// When to send a stream that should go out a fixed number of times a second, in a game whose frames come at any rate.
// Sending only once the interval has passed since the last send skips every frame that comes a little early:
// at 29 fps, with frames of 33-37 ms against a 33 ms interval, that gave 19 sends a second instead of 29.
// Here the sends sit on a grid of slots one interval apart. A frame up to the slack before its slot takes it,
// and the next slot stays on the grid, so a game near the rate sends every frame and a fast one still sends
// at the rate on average. After a pause the grid restarts from now instead of sending every frame to catch up.
namespace SendGrid
{
// The player's own movement and the objects this game moves: thirty times a second, a frame up to 8 ms early still sending.
constexpr std::chrono::milliseconds cMovementInterval{33};
constexpr std::chrono::milliseconds cMovementSlack{8};
static_assert(cMovementSlack * 2 < cMovementInterval, "two sends could come in the same instant");

class Schedule
{
public:
    using Clock = std::chrono::steady_clock;

    constexpr Schedule(const Clock::duration aInterval, const Clock::duration aSlack) noexcept
        : m_interval(aInterval)
        , m_slack(aSlack)
    {
        // Twice the slack must stay under the interval, or two sends could come in the same instant.
        assert(aSlack * 2 < aInterval);
    }

    // Whether to send this frame; a yes takes the slot.
    bool IsDue(const Clock::time_point aNow) noexcept
    {
        if (!m_started)
        {
            m_started = true;
            m_next = aNow + m_interval;
            return true;
        }

        if (aNow < m_next - m_slack)
            return false;

        // Two sends are never closer than the interval less twice the slack.
        m_next = std::max(m_next + m_interval, aNow + m_interval - m_slack);
        return true;
    }

private:
    Clock::duration m_interval;
    Clock::duration m_slack;
    Clock::time_point m_next{};
    bool m_started = false;
};
} // namespace SendGrid
