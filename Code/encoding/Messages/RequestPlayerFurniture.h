#pragma once

#include "Message.h"

#include <Structs/PlayerFurnitureWord.h>

struct RequestPlayerFurniture final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestPlayerFurniture;

    RequestPlayerFurniture()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(Buffer::Writer& aWriter) const noexcept override
    {
        Serialization::WriteVarInt(aWriter, Request.Actor);
        Serialization::WriteVarInt(aWriter, Request.Epoch);
        Serialization::WriteVarInt(aWriter, Request.Sequence);
        Serialization::WriteVarInt(aWriter, static_cast<uint8_t>(Request.Change));
        PlayerFurniture::WriteSeat(aWriter, Request.Where);
        Serialization::WriteVarInt(aWriter, Request.Lease);
        Serialization::WriteVarInt(aWriter, Request.EnterSequence);
        Cell.Serialize(aWriter);
    }

    void DeserializeRaw(Buffer::Reader& aReader) noexcept override
    {
        ClientMessage::DeserializeRaw(aReader);
        Request.Actor = PlayerFurniture::ReadField(aReader);
        Request.Epoch = PlayerFurniture::ReadField(aReader);
        Request.Sequence = Serialization::ReadVarInt(aReader);
        Request.Change = static_cast<PlayerFurniture::Action>(PlayerFurniture::ReadField(aReader, static_cast<uint8_t>(PlayerFurniture::Action::Cancel)));
        Request.Where = PlayerFurniture::ReadSeat(aReader);
        Request.Lease = Serialization::ReadVarInt(aReader);
        Request.EnterSequence = Serialization::ReadVarInt(aReader);
        Cell.Deserialize(aReader);
    }

    bool operator==(const RequestPlayerFurniture& acRhs) const noexcept { return Request == acRhs.Request && Cell == acRhs.Cell; }

    PlayerFurniture::Request Request{};
    GameId Cell{};
};
