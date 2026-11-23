#pragma once

#include "Message.h"

//! A rider got off its horse in its owner's game: the copy gets off too.
struct NotifyDismount final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyDismount;

    NotifyDismount()
        : ServerMessage(Opcode)
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
        ServerMessage::DeserializeRaw(aReader);

        RiderId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        RiderOwnershipEpoch = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        MountId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    }

    bool operator==(const NotifyDismount& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && RiderId == acRhs.RiderId && RiderOwnershipEpoch == acRhs.RiderOwnershipEpoch && MountId == acRhs.MountId;
    }

    uint32_t RiderId{};
    uint32_t RiderOwnershipEpoch{};
    uint32_t MountId{};
};
