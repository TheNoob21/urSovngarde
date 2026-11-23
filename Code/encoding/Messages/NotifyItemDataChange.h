#pragma once

#include "Message.h"

#include <Structs/ItemDataChange.h>

//! Another player's items changed where they lie (ItemDataChange); their copy here changes the same way (ItemDataService).
struct NotifyItemDataChange final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyItemDataChange;

    NotifyItemDataChange()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyItemDataChange& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && OwnershipEpoch == acRhs.OwnershipEpoch && Changes == acRhs.Changes;
    }

    uint32_t ServerId{};
    uint32_t OwnershipEpoch{};
    Vector<ItemDataChange> Changes{};
};
