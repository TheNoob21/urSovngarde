#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// The rules for the other player's bar (the game's enemy health meter), with no game in them.
// The game's meter takes a target that is not hostile only while its health is below full, and drops it again once its
// health has not changed for iEnemyHealthDisplayTime (2 s). So the other player's bar is put up by us, and told again
// before that time runs out (EnemyHealth::SetTarget, the same code on SE and VR).
namespace PlayerMeterRules
{
constexpr float kRange = 3000.f;      // about 40 m
constexpr float kAcquireAngle = 18.f; // degrees off the centre of your view to bring the bar up
constexpr float kHoldAngle = 32.f;    // wider once it is up, so a glance aside does not drop it
constexpr float kHurtFraction = 0.6f; // below this, show them wherever you are looking
constexpr float kBlindRange = 1500.f; // when the view cannot be read (angle below 0)

// Flat: how wide the screen is taken to be, in degrees, to turn a point on it into an angle off its centre.
constexpr float kScreenWidthDegrees = 80.f;
constexpr float kScreenHeightDegrees = 50.f;

// How often our bar is told again: well inside the game's 2 s, and not every send, since each one redraws the name.
constexpr uint64_t kRearmMs = 1000;

//! Degrees off the centre of the screen for a point the camera projected (x and y 0 to 1 across the screen,
//! z below 0 behind the camera). Behind the camera, or not a number: 180, never looked at.
inline float ScreenAngle(const float aX, const float aY, const float aZ) noexcept
{
    if (!std::isfinite(aX) || !std::isfinite(aY) || !std::isfinite(aZ) || aZ < 0.f)
        return 180.f;
    const float cDx = (aX - 0.5f) * kScreenWidthDegrees;
    const float cDy = (aY - 0.5f) * kScreenHeightDegrees;
    return std::sqrt(cDx * cDx + cDy * cDy);
}

//! How much a player is worth showing: 0 for not at all. aAngle below 0 means the view could not be read.
//! Looking at someone beats a scratch across the room; the worse they are hurt, the more it counts.
inline float Score(const float aDistance, const float aAngle, const float aHealthFraction, const bool aHeld) noexcept
{
    if (aDistance > kRange)
        return 0.f;
    const bool cLooking = aAngle < 0.f ? aDistance < kBlindRange : aAngle <= (aHeld ? kHoldAngle : kAcquireAngle);
    const bool cHurt = aHealthFraction < kHurtFraction;
    if (!cLooking && !cHurt)
        return 0.f;
    return (cLooking ? 100.f - std::max(aAngle, 0.f) : 0.f) + (cHurt ? (1.f - aHealthFraction) * 60.f : 0.f);
}

enum class AfterSetTarget
{
    Leave,      // nothing for us to do
    Force,      // the game turned our player down (not hurt): put them up ourselves
    Rearm,      // ours is up: start the game's display time again
    GameTarget, // the game took a target of its own (a hit): our player waits
};

//! What follows the game's own SetTarget. aSent is the handle in the message, aWanted the other player we show
//! (0 for none), aShown the meter's target after the game handled it, aSinceRearmMs the time since we last told it.
inline AfterSetTarget Decide(const uint32_t aSent, const uint32_t aWanted, const uint32_t aShown, const uint64_t aSinceRearmMs) noexcept
{
    if (aSent != 0 && aSent != aWanted)
        return aShown == aSent ? AfterSetTarget::GameTarget : AfterSetTarget::Leave;
    if (aWanted == 0 || aSent != aWanted)
        return AfterSetTarget::Leave;
    // Another target is up (the one the player attacks): it keeps the meter.
    if (aShown != 0 && aShown != aWanted)
        return AfterSetTarget::Leave;
    if (aShown == 0)
        return AfterSetTarget::Force;
    return aSinceRearmMs >= kRearmMs ? AfterSetTarget::Rearm : AfterSetTarget::Leave;
}
} // namespace PlayerMeterRules
