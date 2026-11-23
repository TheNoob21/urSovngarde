#pragma once

#include "Message.h"

#include <Structs/GameId.h>

#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>

//! An arrow of another player's stuck in an actor: the bone it sits on and where on it, relayed by the server from RequestStuckArrow.
struct NotifyStuckArrow final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyStuckArrow;

    NotifyStuckArrow()
        : ServerMessage(Opcode)
    {
    }

    virtual ~NotifyStuckArrow() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyStuckArrow& acRhs) const noexcept
    {
        return ServerId == acRhs.ServerId && Ammo == acRhs.Ammo && Bone == acRhs.Bone && Position == acRhs.Position && Rotation == acRhs.Rotation && Scale == acRhs.Scale &&
               Sequence == acRhs.Sequence && Removed == acRhs.Removed && GetOpcode() == acRhs.GetOpcode();
    }

    //! The actor the arrow hit.
    uint32_t ServerId{};
    //! The ammunition the arrow was.
    GameId Ammo{};
    //! The third-person skeleton node it sits on (StuckArrow::kMaxBoneNameLength at most).
    String Bone{};
    //! Where it sits in the space of that node, the offset before the node's scale.
    glm::vec3 Position{};
    glm::quat Rotation{1.f, 0.f, 0.f, 0.f};
    float Scale{1.f};
    //! Counts the arrows stuck in this actor, so a game can tell the oldest.
    uint32_t Sequence{};
    //! The owner validated removal of this attached arrow's ammunition; monotonic for this sequence.
    bool Removed{};
};
