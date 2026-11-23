#pragma once

#include "Message.h"

//! The party leader fast-travelled; the server tells the members where (its own record of the leader's cell).
struct RequestPartyFastTravel final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestPartyFastTravel;

    RequestPartyFastTravel()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer&) const noexcept override {}

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override { ClientMessage::DeserializeRaw(aReader); }

    bool operator==(const RequestPartyFastTravel& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode(); }
};
