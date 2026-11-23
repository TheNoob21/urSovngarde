#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>

//! Where a remote player's weapon or arrow goes, from the hands that hold it.
//! Plain values in and out: the caller reads the hands from the copy's skeleton and writes the result back.
namespace HeldWeaponPlacement
{
struct Placement
{
    //! Columns are where the item's own x, y and z axes go.
    glm::mat3 Rotation{1.f};
    glm::vec3 Position{};
};

//! Held as the owner holds it: acRotation and acOffset are in the space of the hand bone, the offset before the hand's scale.
inline Placement InHand(const glm::mat3& acHandRotation, const glm::vec3& acHandAt, const float aHandScale, const glm::quat& acRotation, const glm::vec3& acOffset) noexcept
{
    return {acHandRotation * glm::mat3_cast(acRotation), acHandAt + acHandRotation * (acOffset * aHandScale)};
}

//! Hands closer than this are nocking, not drawing: the hand-to-hand line says little about where the bow points.
constexpr float cNockReach = 25.f;
//! From this far apart the hands are a draw and the arrow follows the line between them.
constexpr float cDrawFrom = 40.f;

inline std::optional<glm::vec3> BowDirection(const glm::vec3& acGrip, const glm::vec3& acLowerString, const glm::vec3& acUpperString) noexcept
{
    const glm::vec3 cLimb = acUpperString - acLowerString;
    const float cLength = glm::length(cLimb);
    if (!(cLength > 5.f && cLength < 250.f))
        return std::nullopt;
    const glm::vec3 cAxis = cLimb / cLength;
    const glm::vec3 cAcross = acGrip - (acLowerString + acUpperString) * 0.5f;
    const glm::vec3 cForward = cAcross - cAxis * glm::dot(cAcross, cAxis);
    const float cDepth = glm::length(cForward);
    if (!(cDepth > 2.f && cDepth < 75.f))
        return std::nullopt;
    return cForward / cDepth;
}

namespace Detail
{
//! The arrow's y axis along aForward (unit), its head aNockDistance ahead of the string hand.
inline void Lay(const glm::vec3& acStringAt, const glm::vec3& acForward, const float aNockDistance, Placement& aOut) noexcept
{
    glm::vec3 up{0.f, 0.f, 1.f};
    if (std::abs(glm::dot(up, acForward)) > 0.95f)
        up = glm::vec3{1.f, 0.f, 0.f};
    const glm::vec3 cRight = glm::normalize(glm::cross(acForward, up));
    const glm::vec3 cUp = glm::cross(cRight, acForward);
    aOut.Rotation = glm::mat3(cRight, acForward, cUp);
    aOut.Position = acStringAt + acForward * aNockDistance;
}
} // namespace Detail

//! On the string: the arrow's y axis points from the string hand to the bow hand.
//! Its origin is the head, so it goes aNockDistance ahead of the string hand and the nock lands on the string.
//! False when the hands are too close together to tell which way the arrow points.
inline bool OnString(const glm::vec3& acStringAt, const glm::vec3& acBowAt, const float aNockDistance, Placement& aOut) noexcept
{
    const glm::vec3 cAlong = acBowAt - acStringAt;
    if (!(glm::length(cAlong) > 1.f))
        return false;
    Detail::Lay(acStringAt, glm::normalize(cAlong), aNockDistance, aOut);
    return true;
}

//! As OnString, for hands that may be close together: nocking, the arrow follows the bow's aim when available;
//! drawing, it follows the hands. Between cNockReach and cDrawFrom the two blend.
//! Hands closer than cDrawFrom with the string hand ahead of the bow hand are not a draw either.
//! False when no direction is known (acAim is not a direction and the hands are too close).
inline bool OnStringAimed(const glm::vec3& acStringAt, const glm::vec3& acBowAt, const glm::vec3& acAim, const float aNockDistance, Placement& aOut) noexcept
{
    const glm::vec3 cAlong = acBowAt - acStringAt;
    const float cApart = glm::length(cAlong);
    const bool cHasAim = std::isfinite(acAim.x + acAim.y + acAim.z) && glm::length(acAim) > 0.5f;
    const bool cHasLine = cApart > 1.f && std::isfinite(cApart);
    if (!cHasAim)
        return OnString(acStringAt, acBowAt, aNockDistance, aOut);
    const glm::vec3 cAim = glm::normalize(acAim);
    if (!cHasLine)
    {
        Detail::Lay(acStringAt, cAim, aNockDistance, aOut);
        return true;
    }
    const glm::vec3 cLine = cAlong / cApart;
    float weight = std::clamp((cApart - cNockReach) / (cDrawFrom - cNockReach), 0.f, 1.f);
    if (cApart < cDrawFrom && glm::dot(cLine, cAim) < 0.f)
        weight = 0.f;
    const glm::vec3 cMixed = cAim * (1.f - weight) + cLine * weight;
    const glm::vec3 cForward = glm::length(cMixed) > 0.1f ? glm::normalize(cMixed) : cAim;
    Detail::Lay(acStringAt, cForward, aNockDistance, aOut);
    return true;
}

//! Whether two hands this far apart are a draw (the string follows the string hand), not a nock.
inline bool IsDraw(const float aHandsApart) noexcept
{
    return aHandsApart >= cNockReach;
}

//! The hand that holds the bow, 0 left and 1 right. The arrow is taken in the other one, so the hand it was seen in says it.
//! Else the only hand with a weapon in it (VRPose::WeaponHeld, which a hand the arm did not reach to loses), else the left.
//! aArrowSide: the hand the arrow was last held in, -1 when it has not been seen in one.
inline size_t BowSide(const int aArrowSide, const bool aHasWeapons, const bool aLeftHeld, const bool aRightHeld) noexcept
{
    if (aArrowSide == 0 || aArrowSide == 1)
        return aArrowSide == 0 ? 1 : 0;
    if (aHasWeapons && aLeftHeld != aRightHeld)
        return aRightHeld ? 1 : 0;
    return 0;
}

//! A node's world, as the engine keeps it: rotation, position and a uniform scale.
struct Frame
{
    glm::mat3 Rotation{1.f};
    glm::vec3 Position{};
    float Scale = 1.f;
};

//! A child's world from its parent's world and its own local, the way the engine composes it each frame.
inline Frame Compose(const Frame& acParent, const Frame& acLocal) noexcept
{
    return {acParent.Rotation * acLocal.Rotation, acParent.Position + acParent.Rotation * (acLocal.Position * acParent.Scale), acParent.Scale * acLocal.Scale};
}
} // namespace HeldWeaponPlacement
