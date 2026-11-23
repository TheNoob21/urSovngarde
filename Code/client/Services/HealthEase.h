#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// How the other player's health is shown between their reports, with no game in it.
// Their health reaches this game once a second as a number (ActorValueService), so the bar at the top of the screen (or
// above their head on VR) rose and fell in steps a second apart while they healed or drained. The copy's health is
// walked from where it is to the new number over the time the next report takes, so the bar moves all the time, as the
// game's own does. A blow is not eased: it lands at once, as it does in the game.
namespace HealthEase
{
// A gap of this much or more is a heal or a hit of its own, shown at once (the game's meter animates that itself).
constexpr float kMaxGap = 40.f;
// The time the next report takes.
constexpr double kRampMs = 1000.0;
// The copy's health is set at most this often while it walks: finer steps change nothing the eye sees.
constexpr double kStepMs = 33.0;

//! Which copy a walk is for: the other player's character on the server, the ownership epoch it came with, and the
//! game's actor that shows it here.
struct Binding
{
    uint32_t ServerId{};
    uint32_t Epoch{};
    uint32_t FormId{};

    bool operator==(const Binding& acRhs) const noexcept { return ServerId == acRhs.ServerId && Epoch == acRhs.Epoch && FormId == acRhs.FormId; }
};

struct Ramp
{
    float From{};
    float To{};
    double Start{};
    double Last{};
    Binding For{};
};

//! Whether a walk may still set the health of the copy it was made for. acNow is that copy as it is bound now (a
//! character the server gave another actor, a new epoch, or another form: not the one), and aRemotePlayer says the
//! actor is still another player's copy. Anything else is dropped without a write.
inline bool StillFor(const Ramp& acRamp, const Binding& acNow, const bool aRemotePlayer) noexcept
{
    return aRemotePlayer && acRamp.For.ServerId != 0 && acRamp.For == acNow;
}

//! Whether a new number is walked to: a gap that is small enough, and not nothing.
inline bool Eases(const float aCurrent, const float aTarget) noexcept
{
    if (!std::isfinite(aCurrent) || !std::isfinite(aTarget))
        return false;
    const float cGap = std::abs(aTarget - aCurrent);
    return cGap > 0.f && cGap < kMaxGap;
}

//! The health the walk is at, aNow ms: a straight line from the start to the target over kRampMs.
inline float ValueAt(const Ramp& acRamp, const double aNow) noexcept
{
    const double cT = std::clamp((aNow - acRamp.Start) / kRampMs, 0.0, 1.0);
    return acRamp.From + (acRamp.To - acRamp.From) * static_cast<float>(cT);
}

//! The walk has reached its target.
inline bool IsDone(const Ramp& acRamp, const double aNow) noexcept
{
    return aNow - acRamp.Start >= kRampMs;
}

//! Whether the copy's health is to be set now: the first step at once, the rest every kStepMs, and the last always.
inline bool StepDue(const Ramp& acRamp, const double aNow) noexcept
{
    return aNow - acRamp.Last >= kStepMs || IsDone(acRamp, aNow);
}
} // namespace HealthEase
