#pragma once

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "BodyPoseValidation.h"

// Value-only conversion; each transform is the native row-major 3x3, translation, scale.
namespace BodyPoseWorldInput
{
using Transform = std::array<float, 13>;

inline bool ReadRotation(const Transform& acValue, glm::mat3& aRotation) noexcept
{
    for (float component : acValue)
        if (!std::isfinite(component))
            return false;
    if (acValue[12] <= 0.f)
        return false;
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            aRotation[column][row] = acValue[row * 3 + column];
    return BodyPoseValidation::IsRotation(aRotation);
}

// Reconstruct a private local transform from a physical world transform in game units.
// The local scale belongs to the original transform and is preserved bit-for-bit.
inline bool ReconstructLocal(const Transform& acParentWorld, const Transform& acBodyWorld, const Transform& acOriginalLocal, Transform& aOutput) noexcept
{
    glm::mat3 parent{}, body{}, original{};
    if (!ReadRotation(acParentWorld, parent) || !ReadRotation(acBodyWorld, body) || !ReadRotation(acOriginalLocal, original))
        return false;

    Transform candidate;
    std::memcpy(candidate.data(), acOriginalLocal.data(), sizeof(candidate));
    // Native matrices may contain small accepted drift. Invert the supplied
    // matrix rather than assuming its transpose is its exact inverse.
    const glm::dmat3 inverseParent = glm::inverse(glm::dmat3(parent));
    constexpr double maxFloat = static_cast<double>(std::numeric_limits<float>::max());
    for (int row = 0; row < 3; ++row)
    {
        for (int col = 0; col < 3; ++col)
        {
            double value = 0.0;
            for (int k = 0; k < 3; ++k)
                value += inverseParent[k][row] * acBodyWorld[k * 3 + col];
            if (!std::isfinite(value) || std::fabs(value) > maxFloat)
                return false;
            candidate[row * 3 + col] = static_cast<float>(value);
        }
        double value = 0.0;
        for (int k = 0; k < 3; ++k)
            value += inverseParent[k][row] * (static_cast<double>(acBodyWorld[9 + k]) - acParentWorld[9 + k]);
        value /= static_cast<double>(acParentWorld[12]);
        if (!std::isfinite(value) || std::fabs(value) > maxFloat)
            return false;
        candidate[9 + row] = static_cast<float>(value);
    }
    glm::mat3 result{};
    if (!ReadRotation(candidate, result))
        return false;
    std::memcpy(aOutput.data(), candidate.data(), sizeof(candidate));
    return true;
}

// The output is committed only after validating every input. Translation and scale
// retain their original bit patterns, including signed zero.
inline bool Solve(const Transform& acParentWorld, const Transform& acRootWorld, const Transform& acOriginalLocal, const glm::quat& acSource, Transform& aTargetLocal) noexcept
{
    glm::mat3 parent{}, root{}, original{};
    if (!ReadRotation(acParentWorld, parent) || !ReadRotation(acRootWorld, root) || !ReadRotation(acOriginalLocal, original))
        return false;
    if (!std::isfinite(acSource.w) || !std::isfinite(acSource.x) || !std::isfinite(acSource.y) || !std::isfinite(acSource.z) ||
        std::fabs(glm::dot(acSource, acSource) - 1.f) > 0.002f)
        return false;

    const glm::quat parentQ = glm::normalize(glm::quat_cast(parent));
    const glm::quat rootQ = glm::normalize(glm::quat_cast(root));
    const glm::quat result = glm::normalize(glm::conjugate(parentQ) * rootQ * glm::normalize(acSource));
    const glm::mat3 matrix = glm::mat3_cast(result);
    Transform candidate;
    std::memcpy(candidate.data(), acOriginalLocal.data(), sizeof(candidate));
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            candidate[row * 3 + column] = matrix[column][row];
    std::memcpy(aTargetLocal.data(), candidate.data(), sizeof(candidate));
    return true;
}
} // namespace BodyPoseWorldInput

namespace BodyPoseWorldInput
{
inline bool UnitQuaternion(const glm::quat& acQuaternion) noexcept
{
    return std::isfinite(acQuaternion.w) && std::isfinite(acQuaternion.x) && std::isfinite(acQuaternion.y) && std::isfinite(acQuaternion.z) &&
           std::fabs(glm::dot(acQuaternion, acQuaternion) - 1.f) <= 0.002f;
}

// Break the exact half-turn tie by the first nonzero vector component.
inline glm::quat CanonicalQuaternion(glm::quat aQuaternion) noexcept
{
    if (aQuaternion.w < 0.f || (aQuaternion.w == 0.f && (aQuaternion.x < 0.f || (aQuaternion.x == 0.f && (aQuaternion.y < 0.f || (aQuaternion.y == 0.f && aQuaternion.z < 0.f))))))
        return -aQuaternion;
    return aQuaternion;
}
// Applies a shortest-arc angular envelope and a per-frame angular speed limit.
inline bool StepDelta(const glm::quat& acPreviousDelta, const glm::quat& acDesiredDelta, float aDtSeconds, glm::quat& aResult) noexcept
{
    constexpr float pi = 3.14159265358979323846f;
    constexpr float envelope = 5.f * pi / 180.f;
    constexpr float epsilon = 0.00001f;
    if (!UnitQuaternion(acPreviousDelta) || !UnitQuaternion(acDesiredDelta) || !std::isfinite(aDtSeconds) || aDtSeconds <= 0.f || aDtSeconds > 0.1f)
        return false;
    glm::quat previous = CanonicalQuaternion(glm::normalize(acPreviousDelta));
    glm::quat desired = CanonicalQuaternion(glm::normalize(acDesiredDelta));
    const auto angle = [](const glm::quat& acQuaternion)
    {
        return 2.f * std::atan2(glm::length(glm::vec3(acQuaternion.x, acQuaternion.y, acQuaternion.z)), acQuaternion.w);
    };
    const float previousAngle = angle(previous);
    if (previousAngle > envelope + epsilon)
        return false;
    if (previousAngle > envelope)
        previous = glm::angleAxis(envelope, glm::normalize(glm::vec3(previous.x, previous.y, previous.z)));
    const float desiredAngle = angle(desired);
    if (desiredAngle > envelope)
        desired = glm::angleAxis(envelope, glm::normalize(glm::vec3(desired.x, desired.y, desired.z)));
    if (glm::dot(previous, desired) < 0.f)
        desired = -desired;
    const glm::quat relative = glm::conjugate(previous) * desired;
    const float distance = 2.f * std::atan2(glm::length(glm::vec3(relative.x, relative.y, relative.z)), std::fabs(relative.w));
    const float limit = glm::min(0.25f * pi / 180.f, 15.f * pi / 180.f * glm::min(aDtSeconds, 1.f / 30.f));
    glm::quat result = distance > limit ? glm::slerp(previous, desired, limit / distance) : desired;
    result = glm::normalize(result);
    if (!UnitQuaternion(result))
        return false;
    if (result.w < 0.f)
        result = -result;
    const float resultAngle = angle(result);
    if (resultAngle > envelope + epsilon)
        return false;
    if (resultAngle > envelope)
        result = glm::angleAxis(envelope, glm::normalize(glm::vec3(result.x, result.y, result.z)));
    aResult = result;
    return true;
}

// Native TRS composition using the supplied row-major matrices without normalization.
inline bool ComposeReference(const Transform& acParent, const Transform& acLocal, Transform& aOutput) noexcept
{
    glm::mat3 p{}, l{};
    if (!ReadRotation(acParent, p) || !ReadRotation(acLocal, l))
        return false;
    Transform candidate{};
    for (int row = 0; row < 3; ++row)
    {
        for (int col = 0; col < 3; ++col)
            for (int k = 0; k < 3; ++k)
                candidate[row * 3 + col] += acParent[row * 3 + k] * acLocal[k * 3 + col];
        float translated = 0.f;
        for (int k = 0; k < 3; ++k)
            translated += acParent[row * 3 + k] * acLocal[9 + k];
        candidate[9 + row] = acParent[9 + row] + acParent[12] * translated;
    }
    candidate[12] = acParent[12] * acLocal[12];
    glm::mat3 resultRotation{};
    if (!ReadRotation(candidate, resultRotation))
        return false;
    std::memcpy(aOutput.data(), candidate.data(), sizeof(candidate));
    return true;
}
} // namespace BodyPoseWorldInput
