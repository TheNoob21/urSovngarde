#pragma once

#include <Messages/Message.h>
#include <Structs/ExtensionData.h>

struct NotifyExtension final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyExtension;

    NotifyExtension()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override
    {
        aWriter.WriteBits(Sender, 32);
        Data.Serialize(aWriter);
    }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ServerMessage::DeserializeRaw(aReader);
        uint64_t sender{};
        if (!aReader.ReadBits(sender, 32))
            DecodeGuard::Fail();
        Sender = static_cast<uint32_t>(sender);
        Data.Deserialize(aReader);
        if (!Data.ValidNotification(Sender))
            DecodeGuard::Fail();
    }

    uint32_t Sender{};
    ExtensionData Data;
};
