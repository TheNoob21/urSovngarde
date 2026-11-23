#pragma once

#include "Message.h"

//! The rider's game read back that its rider is off the horse it said it rode.
struct RequestDismount final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestDismount;

    RequestDismount()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override
    {
        Serialization::WriteVarInt(aWriter, RiderId);
        Serialization::WriteVarInt(aWriter, RiderOwnershipEpoch);
        Serialization::WriteVarInt(aWriter, MountId);
    }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ClientMessage::DeserializeRaw(aReader);

        RiderId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        RiderOwnershipEpoch = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        MountId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    }

    bool operator==(const RequestDismount& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && RiderId == acRhs.RiderId && RiderOwnershipEpoch == acRhs.RiderOwnershipEpoch && MountId == acRhs.MountId;
    }

    uint32_t RiderId{};
    uint32_t RiderOwnershipEpoch{};
    uint32_t MountId{};
};
