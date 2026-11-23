#pragma once

#include "Message.h"

#include <Structs/RagdollPose.h>

//! Where another game has the bodies of a ragdoll it runs (a relayed RequestRagdollPose); the receiver sets its copy's into them.
struct NotifyRagdollPose final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyRagdollPose;

    NotifyRagdollPose()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyRagdollPose& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && OwnershipEpoch == acRhs.OwnershipEpoch && Pose == acRhs.Pose;
    }

    //! Server id of the actor.
    uint32_t ServerId{};
    uint32_t OwnershipEpoch{};
    RagdollPose Pose{};
};
