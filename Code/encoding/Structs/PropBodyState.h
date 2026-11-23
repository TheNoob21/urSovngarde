#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

//! One moving Havok body of a placed object (a hanging sign's board on its hinge), in the space of the object's 3D root.
struct PropBodyPose
{
    bool operator==(const PropBodyPose& acRhs) const noexcept;
    bool operator!=(const PropBodyPose& acRhs) const noexcept { return !(*this == acRhs); }

    //! Game units, in the root's space with the root's scale taken out.
    glm::vec3 Position{};
    //! Relative to the root's rotation.
    glm::quat Rotation{1.f, 0.f, 0.f, 0.f};
};

//! A placed object's moving bodies, in the order its 3D lists them.
struct PropBodyState
{
    //! A message with more bodies than this is dropped.
    static constexpr size_t kMaxBodies = 16;
    //! Positions travel in sixteenths of a unit, so a body this far from its root is the most there is room for.
    static constexpr float kMaxDistance = 2000.f;

    bool operator==(const PropBodyState& acRhs) const noexcept { return Layout == acRhs.Layout && Bodies == acRhs.Bodies; }
    bool operator!=(const PropBodyState& acRhs) const noexcept { return !(*this == acRhs); }

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    //! Too many bodies fails the message (DecodeGuard).
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    uint32_t Layout{};
    TiltedPhoques::Vector<PropBodyPose> Bodies{};
};
