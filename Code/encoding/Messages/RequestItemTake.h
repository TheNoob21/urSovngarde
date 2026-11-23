#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/ItemTakeSource.h>
#include <Structs/Vector3_NetQuantize.h>

//! The player picked up a dropped or a placed item another player may take at the same time.
//! The server gives it to the first player who took it.
struct RequestItemTake final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestItemTake;

    RequestItemTake()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestItemTake& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && From == acRhs.From && Id == acRhs.Id && ObjectId == acRhs.ObjectId && CellId == acRhs.CellId &&
               WorldSpaceId == acRhs.WorldSpaceId && Position == acRhs.Position;
    }

    //! kDroppedItem or kPlacedItem.
    ItemTakeSource From{};
    //! The dropped item's id.
    uint32_t Id{};
    //! A placed item's own reference id, the same in every game that has the plugin it comes from, and its cell.
    GameId ObjectId{};
    GameId CellId{};
    GameId WorldSpaceId{};
    Vector3_NetQuantize Position{};
};
