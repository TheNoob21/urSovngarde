#pragma once

#include <chrono>
#include <cstdint>

//! When a chest's lid, opened here for another player who opened that chest in their game, is closed again.
//! The other game sends the chest's use, not when its menu closes, so the lid closes once that player's character walks
//! away from it, is gone, or after a long while. Pure rules on plain values, so they can be tested (tests/container_lid_rules.cpp).
namespace ContainerLidRules
{
//! Farther than this from the chest, its user has left it.
constexpr float kLeftAt = 300.f;
//! Open for this long, the lid closes whatever its user does: a menu is not kept open that long.
constexpr auto kOpenAtMost = std::chrono::minutes(3);

//! aUserHere: the character that opened it is still here. aDistance: how far it is from the chest.
//! aOpenFor: how long the lid has been open.
inline bool ShouldClose(const bool aUserHere, const float aDistance, const std::chrono::steady_clock::duration aOpenFor) noexcept
{
    return !aUserHere || !(aDistance <= kLeftAt) || aOpenFor >= kOpenAtMost;
}
} // namespace ContainerLidRules
