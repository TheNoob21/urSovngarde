#include <catch2/catch.hpp>

#include <Services/SendGrid.h>

#include <algorithm>
#include <chrono>
#include <vector>

using namespace std::chrono_literals;

namespace
{
using Clock = std::chrono::steady_clock;

// The player's movement and the held objects both use this schedule.
constexpr auto cInterval = SendGrid::cMovementInterval;
constexpr auto cSlack = SendGrid::cMovementSlack;

struct Run
{
    double SendsPerSecond = 0.0;
    double FramesPerSecond = 0.0;
    Clock::duration ShortestGap = Clock::duration::max();
    Clock::duration LongestGap{};
};

// Plays the frame times given, repeated for the whole length, through a schedule and counts what it sends.
template <class Gate> Run Play(Gate aGate, const std::vector<Clock::duration>& acFrames, const Clock::duration aLength)
{
    Run run{};
    const auto cStart = Clock::time_point{} + 1h;
    auto now = cStart;
    auto lastSend = Clock::time_point{};
    size_t sends = 0, frames = 0;
    for (size_t i = 0; now - cStart < aLength; ++i)
    {
        ++frames;
        if (aGate(now))
        {
            if (sends > 0)
            {
                run.ShortestGap = std::min(run.ShortestGap, now - lastSend);
                run.LongestGap = std::max(run.LongestGap, now - lastSend);
            }
            lastSend = now;
            ++sends;
        }
        now += acFrames[i % acFrames.size()];
    }
    const double cSeconds = std::chrono::duration<double>(now - cStart).count();
    run.SendsPerSecond = static_cast<double>(sends) / cSeconds;
    run.FramesPerSecond = static_cast<double>(frames) / cSeconds;
    return run;
}

Run PlayGrid(const std::vector<Clock::duration>& acFrames, const Clock::duration aLength = 60s)
{
    SendGrid::Schedule schedule{cInterval, cSlack};
    return Play([&schedule](const Clock::time_point aNow) { return schedule.IsDue(aNow); }, acFrames, aLength);
}

// The gate this replaces: send once the interval has passed since the last send.
Run PlayOldGate(const std::vector<Clock::duration>& acFrames, const Clock::duration aLength = 60s)
{
    Clock::time_point last{};
    return Play(
        [&last](const Clock::time_point aNow)
        {
            if (aNow - last < cInterval)
                return false;
            last = aNow;
            return true;
        },
        acFrames, aLength);
}

Clock::duration FrameAt(const double aFps)
{
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / aFps));
}

// Quest frames around 29 fps: most a little over 33 ms, every other one a little under, as an uneven frame pace gives.
std::vector<Clock::duration> QuestFrames()
{
    return {36ms, 32ms, 37ms, 31ms, 35ms, 33ms, 38ms, 32ms, 36ms, 34ms};
}
} // namespace

TEST_CASE("the first frame sends", "[send_grid]")
{
    SendGrid::Schedule schedule{cInterval, cSlack};
    REQUIRE(schedule.IsDue(Clock::time_point{} + 5s));
}

TEST_CASE("a fast PC sends about thirty times a second, never in bursts", "[send_grid]")
{
    for (const double fps : {60.0, 75.0, 90.0, 120.0, 144.0, 240.0})
    {
        CAPTURE(fps);
        const Run run = PlayGrid({FrameAt(fps)});
        REQUIRE(run.SendsPerSecond > 29.0);
        REQUIRE(run.SendsPerSecond < 31.0);
        // No two sends closer than the interval less twice the slack, and none further apart than the interval and a frame.
        REQUIRE(run.ShortestGap >= cInterval - 2 * cSlack);
        REQUIRE(run.LongestGap <= cInterval + FrameAt(fps) + 1ms);
    }
}

TEST_CASE("the old gate sent fewer than thirty a second on a 75 Hz PC", "[send_grid]")
{
    // At 60 fps two frames are 33.3 ms, so every other frame: 30 a second. At 75 fps two frames are 26.7 ms, too short, so every third: 25.
    REQUIRE(PlayOldGate({FrameAt(60.0)}).SendsPerSecond == Approx(30.0).margin(0.5));
    REQUIRE(PlayOldGate({FrameAt(75.0)}).SendsPerSecond == Approx(25.0).margin(0.5));
    REQUIRE(PlayGrid({FrameAt(75.0)}).SendsPerSecond == Approx(30.0).margin(1.0));
}

TEST_CASE("a Quest at an even 24 to 30 fps still sends every frame, as the old gate did", "[send_grid]")
{
    for (const double fps : {24.0, 26.0, 28.0, 30.0})
    {
        CAPTURE(fps);
        const Run run = PlayGrid({FrameAt(fps)});
        REQUIRE(run.SendsPerSecond == Approx(fps).epsilon(0.01));
        REQUIRE(run.SendsPerSecond == Approx(PlayOldGate({FrameAt(fps)}).SendsPerSecond).epsilon(0.01));
        REQUIRE(run.LongestGap <= FrameAt(fps) + 1ms);
    }
}

TEST_CASE("a Quest at 32 to 36 fps sends close to thirty times a second, where the old gate sent every other frame", "[send_grid]")
{
    for (const double fps : {32.0, 34.0, 36.0})
    {
        CAPTURE(fps);
        const Run run = PlayGrid({FrameAt(fps)});
        REQUIRE(run.SendsPerSecond > 27.5);
        REQUIRE(run.SendsPerSecond <= std::min(fps, 31.0));
        // Faster than the grid, a frame is skipped now and then: never more than one in a row (PlaybackClock waits out that gap).
        REQUIRE(run.LongestGap <= 2 * FrameAt(fps) + 1ms);
        REQUIRE(PlayOldGate({FrameAt(fps)}).SendsPerSecond == Approx(fps / 2.0).epsilon(0.02));
    }
}

TEST_CASE("the Quest's uneven frames near 33 ms send every frame, where the old gate skipped the early ones", "[send_grid]")
{
    const Run grid = PlayGrid(QuestFrames());
    const Run old = PlayOldGate(QuestFrames());
    CAPTURE(grid.SendsPerSecond, old.SendsPerSecond, grid.FramesPerSecond);
    REQUIRE(grid.SendsPerSecond == Approx(grid.FramesPerSecond).epsilon(0.01));
    REQUIRE(old.SendsPerSecond < 0.8 * grid.SendsPerSecond);
    // No send waits two frames.
    REQUIRE(grid.LongestGap <= 38ms);
}

TEST_CASE("the other actors' snapshot, looked at only on a player send, still goes ten times a second", "[send_grid]")
{
    // As in RunLocalUpdates: the 100 ms grid is asked only on frames the player's grid sends.
    for (const double fps : {29.0, 36.0, 60.0, 120.0, 144.0, 240.0})
    {
        CAPTURE(fps);
        SendGrid::Schedule player{cInterval, cSlack};
        SendGrid::Schedule full{100ms, cInterval};
        const auto cStart = Clock::time_point{} + 1h;
        size_t fullSends = 0;
        auto now = cStart;
        for (; now - cStart < 60s; now += FrameAt(fps))
        {
            if (player.IsDue(now) && full.IsDue(now))
                ++fullSends;
        }
        const double cPerSecond = static_cast<double>(fullSends) / std::chrono::duration<double>(now - cStart).count();
        REQUIRE(cPerSecond > 9.5);
        REQUIRE(cPerSecond <= 10.1);
    }
}

TEST_CASE("after a pause the grid starts again instead of catching up", "[send_grid]")
{
    SendGrid::Schedule schedule{cInterval, cSlack};
    auto now = Clock::time_point{} + 1h;
    REQUIRE(schedule.IsDue(now));
    // A loading screen: half a second with no frame.
    now += 500ms;
    REQUIRE(schedule.IsDue(now));
    // Fast frames right after it do not all send: the next slot is 25 ms on, so 7 ms frames send at 21 ms, then at 56.
    const auto cResumed = now;
    std::vector<Clock::duration> sentAt;
    for (int i = 0; i < 10; ++i)
    {
        now += 7ms;
        if (schedule.IsDue(now))
            sentAt.push_back(now - cResumed);
    }
    REQUIRE(sentAt == std::vector<Clock::duration>{21ms, 56ms});
}
