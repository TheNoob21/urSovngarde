#pragma once

#include "Message.h"

#include <Structs/RagdollPose.h>

//! Where the bodies of a ragdoll this game runs are, while it is moved: a creature the humanoid pose cannot bend.
//! Sent at most ten times a second; the server relays it as NotifyRagdollPose.
struct RequestRagdollPose final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestRagdollPose;

    RequestRagdollPose()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestRagdollPose& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && OwnershipEpoch == acRhs.OwnershipEpoch && Pose == acRhs.Pose;
    }

    //! Server id of the actor.
    uint32_t ServerId{};
    uint32_t OwnershipEpoch{};
    RagdollPose Pose{};
};
