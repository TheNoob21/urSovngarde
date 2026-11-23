#pragma once

#include <Systems/BodyPosePresentation.h>

// A hand further from the shoulder than the copy's arm is long: the shoulder is brought towards it first by turning the
// clavicle, as someone reaching out does, so the arm's own fine-tuning (BodyPosePresentation::ReachHand) can land the hand
// on it instead of stopping short with the arm straight. Value-only, like ReachHand.
namespace HandReach
{
// About as far as a shoulder rolls forward or up.
inline constexpr float kMaxTurn = 0.5235988f; // 30 degrees
// ReachHand keeps the arm this much short of straight.
inline constexpr float kStraightSlack = 0.5f;

// Turns the clavicle just enough for the hand target to come within the arm's reach, up to the supplied turn limit.
// Returns the turn in radians: 0 when the target is within reach, too far off to be fine-tuning, or the bones are not
// an arm.
inline float TurnClavicle(
    BodyPosePresentation::Output& aOutput, const std::array<int, BodyPosePresentation::kMaxNodes>& acParent, const int aClavicle, const int aUpper, const int aFore,
    const int aHand, const glm::vec3& acTarget, const float aMaxCorrection = BodyPosePresentation::kReachMaxCorrection, const float aMaxTurn = kMaxTurn) noexcept
{
    using namespace BodyPosePresentation;
    const auto valid = [&aOutput](const int aNode) noexcept
    {
        return aNode >= 0 && static_cast<size_t>(aNode) < aOutput.Count;
    };
    if (!valid(aClavicle) || !valid(aUpper) || !valid(aFore) || !valid(aHand) || !Finite(acTarget) || !(aMaxTurn > 0.f) ||
        !Below(acParent, aOutput.Count, static_cast<size_t>(aUpper), aClavicle) || !Below(acParent, aOutput.Count, static_cast<size_t>(aFore), aUpper) ||
        !Below(acParent, aOutput.Count, static_cast<size_t>(aHand), aFore))
        return 0.f;

    const auto at = [&aOutput](const int aNode) noexcept
    {
        return aOutput.world[static_cast<size_t>(aNode)].Translation;
    };
    const glm::vec3 pivot = at(aClavicle), shoulder = at(aUpper), elbow = at(aFore), wrist = at(aHand);
    // The same limit as ReachHand: a bigger gap is a disagreement about where the body is, not a reach.
    const float gap = glm::distance(acTarget, wrist);
    if (!std::isfinite(gap) || gap > aMaxCorrection)
        return 0.f;
    const float upperLength = glm::distance(shoulder, elbow), foreLength = glm::distance(elbow, wrist);
    if (!(upperLength >= 1.f) || !(foreLength >= 1.f))
        return 0.f;
    const float armLength = upperLength + foreLength - kStraightSlack;
    if (glm::distance(acTarget, shoulder) <= armLength)
        return 0.f;

    const glm::vec3 from = shoulder - pivot, to = acTarget - pivot;
    const glm::vec3 axis = glm::cross(from, to);
    const float axisLength = glm::length(axis);
    if (!(glm::length(from) >= 1.f) || !(axisLength >= 1e-4f))
        return 0.f;
    const glm::vec3 unitAxis = axis / axisLength;
    const float full = std::atan2(axisLength, glm::dot(from, to));
    const auto reachAfter = [&](const float aAngle) noexcept
    {
        return glm::distance(acTarget, pivot + glm::angleAxis(aAngle, unitAxis) * from);
    };

    // Turning towards the target only brings the shoulder nearer, so the smallest turn that is enough is found by halving.
    float angle = std::min(full, aMaxTurn);
    if (reachAfter(angle) < armLength)
    {
        float low = 0.f, high = angle;
        for (int i = 0; i < 24; ++i)
        {
            const float middle = 0.5f * (low + high);
            (reachAfter(middle) > armLength ? low : high) = middle;
        }
        angle = high;
    }
    // The turn carries the whole arm round, so the wrist may swing away from the target: it must stay within the
    // fine-tuning range, or ReachHand would leave the arm as it is and the turn would only make the hand worse.
    const auto wristAfter = [&](const float aTurn) noexcept
    {
        return glm::distance(acTarget, pivot + glm::angleAxis(aTurn, unitAxis) * (wrist - pivot));
    };
    // The largest turn that keeps it there, found by halving the interval (not the angle, which could jump by half a
    // turn from one frame to the next as the hand crosses the limit).
    if (std::isfinite(angle) && angle > 0.f && wristAfter(angle) > aMaxCorrection)
    {
        float low = 0.f, high = angle;
        for (int i = 0; i < 24; ++i)
        {
            const float middle = 0.5f * (low + high);
            (wristAfter(middle) > aMaxCorrection ? high : low) = middle;
        }
        angle = low;
    }
    if (!std::isfinite(angle) || !(angle > 1e-4f) || wristAfter(angle) > aMaxCorrection)
        return 0.f;
    TurnBone(aOutput, acParent, aClavicle, glm::angleAxis(angle, unitAxis));
    return angle;
}
} // namespace HandReach
