#pragma once

#include <chrono>
#include <optional>

struct PendingVoice
{
    using Clock = std::chrono::steady_clock;

    bool Expired(Clock::time_point aNow) const noexcept { return aNow - FirstAsked.value_or(FirstSeen) >= std::chrono::seconds(3); }

    bool WaitingForLip(Clock::time_point aNow) const noexcept { return FirstAsked && aNow - *FirstAsked < std::chrono::seconds(1); }

    Clock::time_point FirstSeen;
    std::optional<Clock::time_point> FirstAsked{};
};
