#pragma once

#include "Message.h"

//! Sent after a save is loaded while connected. Carries nothing.
struct RequestSaveLoaded final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestSaveLoaded;

    RequestSaveLoaded()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer&) const noexcept override {}

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override { ClientMessage::DeserializeRaw(aReader); }

    bool operator==(const RequestSaveLoaded& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode(); }
};
