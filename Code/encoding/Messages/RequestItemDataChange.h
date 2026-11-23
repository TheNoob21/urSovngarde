#pragma once

#include "Message.h"

#include <Structs/ItemDataChange.h>

//! A player's own items changed where they lie (ItemDataChange); the server keeps it and tells the others (ItemDataService).
struct RequestItemDataChange final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestItemDataChange;

    RequestItemDataChange()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestItemDataChange& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && OwnershipEpoch == acRhs.OwnershipEpoch && Changes == acRhs.Changes;
    }

    uint32_t ServerId{};
    uint32_t OwnershipEpoch{};
    Vector<ItemDataChange> Changes{};
};
