#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/GridCellCoords.h>
#include <Structs/ReferenceState.h>
#include <Structs/Vector3_NetQuantize.h>

//! A world reference's state this player's game changed (ReferenceStates.h), for the players near it and those who come later.
struct RequestReferenceState final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestReferenceState;

    RequestReferenceState()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override
    {
        CellId.Serialize(aWriter);
        WorldSpaceId.Serialize(aWriter);
        CenterCoords.Serialize(aWriter);
        Position.Serialize(aWriter);
        State.Serialize(aWriter);
    }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ClientMessage::DeserializeRaw(aReader);
        CellId.Deserialize(aReader);
        WorldSpaceId.Deserialize(aReader);
        CenterCoords.Deserialize(aReader);
        Position.Deserialize(aReader);
        State.Deserialize(aReader);
    }

    bool operator==(const RequestReferenceState& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && CellId == acRhs.CellId && WorldSpaceId == acRhs.WorldSpaceId && CenterCoords == acRhs.CenterCoords &&
               Position == acRhs.Position && State == acRhs.State;
    }

    //! The reference's cell; outdoors also its worldspace and grid cell, none indoors.
    GameId CellId{};
    GameId WorldSpaceId{};
    GridCellCoords CenterCoords{};
    //! Where the reference is, as the sender's game has it: the server checks the sender's character stood near it.
    Vector3_NetQuantize Position{};
    ReferenceState State{};
};
