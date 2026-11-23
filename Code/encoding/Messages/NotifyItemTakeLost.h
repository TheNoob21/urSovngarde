#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/Inventory.h>
#include <Structs/ItemTakeSource.h>

//! Another player took the item first: the receiver's game takes it back off its player.
struct NotifyItemTakeLost final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyItemTakeLost;

    NotifyItemTakeLost()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyItemTakeLost& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && From == acRhs.From && Id == acRhs.Id && ObjectId == acRhs.ObjectId && Item == acRhs.Item;
    }

    ItemTakeSource From{};
    //! The container's or body's server id, or the dropped item's id.
    uint32_t Id{};
    //! A placed item's reference id.
    GameId ObjectId{};
    //! Out of a container or a body: the item, and how many of the take the server could not give.
    Inventory::Entry Item{};
};
