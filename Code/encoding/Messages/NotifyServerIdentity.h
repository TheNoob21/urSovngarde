#pragma once

#include "Message.h"

struct NotifyServerIdentity final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyServerIdentity;

    NotifyServerIdentity()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override
    {
        Serialization::WriteString(aWriter, Name);
        Serialization::WriteVarInt(aWriter, Players);
    }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ServerMessage::DeserializeRaw(aReader);
        Name = Serialization::ReadString(aReader);
        Players = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
    }

    bool operator==(const NotifyServerIdentity& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Name == acRhs.Name && Players == acRhs.Players; }

    String Name;
    uint32_t Players{};
};
