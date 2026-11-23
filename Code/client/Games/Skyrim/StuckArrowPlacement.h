#pragma once

#include "HeldWeaponPlacement.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

//! Where an arrow stuck in an actor sits, in the space of the bone it is stuck in, and back to the world.
//! Plain values in and out: the caller reads the bone and the arrow from the skeleton and writes the result back.
//! The convention is the engine's own (HeldWeaponPlacement::Compose): world = bone.Position + bone.Rotation * (local.Position * bone.Scale),
//! rotation = bone.Rotation * local.Rotation, scale = bone.Scale * local.Scale.
namespace StuckArrowPlacement
{
using Frame = HeldWeaponPlacement::Frame;

//! The arrow's place in the space of the bone it is stuck in: what FromBoneLocal turns back into the arrow's world.
//! The bone's scale must not be 0.
inline Frame ToBoneLocal(const Frame& acBoneWorld, const Frame& acArrowWorld) noexcept
{
    const glm::mat3 cInverse = glm::transpose(acBoneWorld.Rotation);
    Frame local{};
    local.Rotation = cInverse * acArrowWorld.Rotation;
    local.Position = (cInverse * (acArrowWorld.Position - acBoneWorld.Position)) / acBoneWorld.Scale;
    local.Scale = acArrowWorld.Scale / acBoneWorld.Scale;
    return local;
}

//! The arrow's world from the bone's world and the place sent for it.
inline Frame FromBoneLocal(const Frame& acBoneWorld, const Frame& acLocal) noexcept
{
    return HeldWeaponPlacement::Compose(acBoneWorld, acLocal);
}

//! The rotation as it goes on the wire.
inline glm::quat ToQuaternion(const glm::mat3& acRotation) noexcept
{
    return glm::normalize(glm::quat_cast(acRotation));
}

inline glm::mat3 ToMatrix(const glm::quat& acRotation) noexcept
{
    return glm::mat3_cast(glm::normalize(acRotation));
}
} // namespace StuckArrowPlacement
