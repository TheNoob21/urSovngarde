#pragma once

#include "Message.h"

#include <Structs/GameId.h>

#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>

//! An arrow of this player's stuck in an actor this game runs: the bone it sits on and where on it, for the other games to show it there.
//! The server relays it as NotifyStuckArrow and keeps nothing, as the game saves no stuck arrows either.
struct RequestStuckArrow final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestStuckArrow;

    RequestStuckArrow()
        : ClientMessage(Opcode)
    {
    }

    virtual ~RequestStuckArrow() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestStuckArrow& acRhs) const noexcept
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
