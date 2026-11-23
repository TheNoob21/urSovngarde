#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/GridCellCoords.h>

//! A trap this player's game set off: its trigger fired it after this game's actor stepped on it or used it (TrapSync).
struct RequestTrapFire final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestTrapFire;

    RequestTrapFire()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override
    {
        Id.Serialize(aWriter);
        TriggerId.Serialize(aWriter);
        CellId.Serialize(aWriter);
        WorldSpaceId.Serialize(aWriter);
        CenterCoords.Serialize(aWriter);
    }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ClientMessage::DeserializeRaw(aReader);
        Id.Deserialize(aReader);
        TriggerId.Deserialize(aReader);
        CellId.Deserialize(aReader);
        WorldSpaceId.Deserialize(aReader);
        CenterCoords.Deserialize(aReader);
    }

    bool operator==(const RequestTrapFire& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Id == acRhs.Id && TriggerId == acRhs.TriggerId && CellId == acRhs.CellId && WorldSpaceId == acRhs.WorldSpaceId &&
               CenterCoords == acRhs.CenterCoords;
    }

    //! The trap, the trigger that fired it, and the trap's cell.
    GameId Id{};
    GameId TriggerId{};
    GameId CellId{};
    //! Outdoors, the trap's worldspace and grid cell; none indoors.
    GameId WorldSpaceId{};
    GridCellCoords CenterCoords{};
};
