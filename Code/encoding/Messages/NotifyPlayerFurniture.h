#pragma once

#include "Message.h"

#include <Structs/PlayerFurnitureWord.h>

struct NotifyPlayerFurniture final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPlayerFurniture;

    NotifyPlayerFurniture()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(Buffer::Writer& aWriter) const noexcept override
    {
        Serialization::WriteVarInt(aWriter, Actor);
        Serialization::WriteVarInt(aWriter, Epoch);
        Serialization::WriteVarInt(aWriter, Sequence);
        Serialization::WriteVarInt(aWriter, Lease);
        PlayerFurniture::WriteSeat(aWriter, Where);
        Cell.Serialize(aWriter);
        Serialization::WriteVarInt(aWriter, static_cast<uint8_t>(Decision));
        Serialization::WriteVarInt(aWriter, static_cast<uint8_t>(State));
    }

    void DeserializeRaw(Buffer::Reader& aReader) noexcept override
    {
        ServerMessage::DeserializeRaw(aReader);
        Actor = PlayerFurniture::ReadField(aReader);
        Epoch = PlayerFurniture::ReadField(aReader);
        Sequence = Serialization::ReadVarInt(aReader);
        Lease = Serialization::ReadVarInt(aReader);
        Where = PlayerFurniture::ReadSeat(aReader);
        Cell.Deserialize(aReader);
        Decision = static_cast<PlayerFurniture::Verdict>(PlayerFurniture::ReadField(aReader, static_cast<uint8_t>(PlayerFurniture::Verdict::Released)));
        using PlayerFurniture::SeatState;
        State = static_cast<SeatState>(PlayerFurniture::ReadField(aReader, static_cast<uint8_t>(SeatState::Sleeping)));
        if (State != SeatState::Standing && State != SeatState::Sitting && State != SeatState::Sleeping)
            DecodeGuard::Fail();
    }

    bool operator==(const NotifyPlayerFurniture& acRhs) const noexcept
    {
        return Actor == acRhs.Actor && Epoch == acRhs.Epoch && Sequence == acRhs.Sequence && Lease == acRhs.Lease && Where == acRhs.Where && Cell == acRhs.Cell &&
               Decision == acRhs.Decision && State == acRhs.State;
    }

    uint32_t Actor{};
    uint32_t Epoch{};
    //! Only a reply to the owner echoes the request sequence.
    uint64_t Sequence{};
    uint64_t Lease{};
    PlayerFurniture::Seat Where{};
    GameId Cell{};
    PlayerFurniture::Verdict Decision{};
    PlayerFurniture::SeatState State{};
};
