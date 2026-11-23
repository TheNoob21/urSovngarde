#pragma once

#include "Message.h"

//! A member takes the leader's fast travel offer; the server answers with a NotifyTeleport to where the leader is now.
struct RequestPartyFollow final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestPartyFollow;

    RequestPartyFollow()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer&) const noexcept override {}

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override { ClientMessage::DeserializeRaw(aReader); }

    bool operator==(const RequestPartyFollow& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode(); }
};
