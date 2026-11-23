#pragma once

#include <algorithm>

#include <glm/glm.hpp>

// Whether something moves with a hand of this player's: both moved, and about the same way. A hand that hits or pushes a thing
// leaves it moving its own way. Tested with HeldObject::Carry and HandHold (Code/tests).
namespace HandMotion
{
//! A step shorter than this (units) is standing still, not moving together.
inline constexpr float kMinStep = 1.f;
//! How far the thing's step may be from the hand's: this much, and this share of the longer step, as it turns in the hand.
inline constexpr float kStepSlack = 2.f;
inline constexpr float kStepShare = 0.5f;

//! Whether a thing that moved by acStep moved with a hand that moved by acHandStep over the same time.
[[nodiscard]] inline bool MovesWith(const glm::vec3& acHandStep, const glm::vec3& acStep) noexcept
{
    const float cHand = glm::length(acHandStep);
    const float cThing = glm::length(acStep);
    if (!(cHand >= kMinStep) || !(cThing >= kMinStep))
        return false;
    return glm::distance(acHandStep, acStep) <= kStepSlack + kStepShare * std::max(cHand, cThing);
}
} // namespace HandMotion
