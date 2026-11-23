#pragma once

#include <cmath>

namespace BodyPoseValidation
{
// NiTransform rotation excludes scale. Allow small floating-point drift, but reject
// partial writes, scale/shear and reflections before converting a matrix to a quaternion.
template <class Matrix> bool IsRotation(const Matrix& acRotation) noexcept
{
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            if (!std::isfinite(acRotation[c][r]))
                return false;
    const auto dot = [&](int aLeft, int aRight)
    {
        return acRotation[aLeft][0] * acRotation[aRight][0] + acRotation[aLeft][1] * acRotation[aRight][1] + acRotation[aLeft][2] * acRotation[aRight][2];
    };
    for (int i = 0; i < 3; ++i)
        if (std::abs(dot(i, i) - 1.f) > 0.05f)
            return false;
    if (std::abs(dot(0, 1)) > 0.05f || std::abs(dot(0, 2)) > 0.05f || std::abs(dot(1, 2)) > 0.05f)
        return false;
    const float determinant = acRotation[0][0] * (acRotation[1][1] * acRotation[2][2] - acRotation[2][1] * acRotation[1][2]) -
                              acRotation[1][0] * (acRotation[0][1] * acRotation[2][2] - acRotation[2][1] * acRotation[0][2]) +
                              acRotation[2][0] * (acRotation[0][1] * acRotation[1][2] - acRotation[1][1] * acRotation[0][2]);
    return std::isfinite(determinant) && determinant >= 0.9f && determinant <= 1.1f;
}
} // namespace BodyPoseValidation
