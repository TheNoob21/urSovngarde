#pragma once

#include <Structs/GameId.h>
#include <DecodeGuard.h>
#include <TiltedCore/Serialization.hpp>

//! A world reference's state that one player's game changed (ReferenceStates.h): a destructible's stage, a bear trap armed or
//! sprung, a plant or a rune used up.
struct ReferenceState
{
    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
    {
        Id.Serialize(aWriter);
        aWriter.WriteBits(Kind, 8);
        aWriter.WriteBits(Value, 8);
        Serialization::WriteVarInt(aWriter, IdleHours);
    }

    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
    {
        Id.Deserialize(aReader);
        uint64_t bits{};
        if (!aReader.ReadBits(bits, 8))
        {
            DecodeGuard::Fail();
            return;
        }
        Kind = static_cast<uint8_t>(bits);
        if (!aReader.ReadBits(bits, 8))
        {
            DecodeGuard::Fail();
            return;
        }
        Value = static_cast<uint8_t>(bits);
        IdleHours = static_cast<uint32_t>(Serialization::ReadVarInt(aReader) & 0xFFFFFFFF);
    }

    bool operator==(const ReferenceState& acRhs) const noexcept { return Id == acRhs.Id && Kind == acRhs.Kind && Value == acRhs.Value && IdleHours == acRhs.IdleHours; }

    GameId Id{};
    //! ReferenceStates::Kind; a kind the receiver does not know is passed over.
    uint8_t Kind{};
    uint8_t Value{};
    //! Kept states sent to a player who came later: game hours since no player had the reference's place loaded. 0 in a
    //! report and in a live relay.
    uint32_t IdleHours{};
};
