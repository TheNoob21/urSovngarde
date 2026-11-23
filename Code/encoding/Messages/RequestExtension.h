#pragma once

#include <Messages/Message.h>
#include <Structs/ExtensionData.h>

struct RequestExtension final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestExtension;

    RequestExtension()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { Data.Serialize(aWriter); }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ClientMessage::DeserializeRaw(aReader);
        Data.Deserialize(aReader);
        if (!Data.ValidRequest())
            DecodeGuard::Fail();
    }

    ExtensionData Data;
};
