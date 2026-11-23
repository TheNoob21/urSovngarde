#pragma once

#include <cmath>

#include <glm/glm.hpp>

// The angles a reference holds (TESObjectREFR::rotation, as SetRotation takes them) for a turn of its 3D.
// Plain values in and out, so the angle order has a unit test (Code/tests/droppeditems.cpp).
namespace ReferenceAngles
{
// The game turns a reference's 3D from its angles as Rx(-x) * Ry(-y) * Rz(-z), with X right, Y forward and Z up
// (each angle clockwise; measured in game against GetAngleX/Y/Z on items set to many mixed poses).
// acTurn is in glm's column-major order, so acTurn[column][row].
inline glm::vec3 FromTurn(const glm::mat3& acTurn) noexcept
{
    const float cSinY = glm::clamp(-acTurn[2][0], -1.f, 1.f);
    const float cY = std::asin(cSinY);

    // Straight up or down, X and Z turn about the same axis: all of it is put in Z.
    if (std::abs(cSinY) > 0.9999999f)
        return {0.f, cY, std::atan2(-acTurn[0][1], acTurn[1][1])};

    return {std::atan2(acTurn[2][1], acTurn[2][2]), cY, std::atan2(acTurn[1][0], acTurn[0][0])};
}
} // namespace ReferenceAngles
