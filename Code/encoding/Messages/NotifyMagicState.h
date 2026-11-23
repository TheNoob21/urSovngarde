#pragma once

#include "Message.h"

#include <Structs/MagicState.h>

//! What another player casts and wears as magic: when it changes, and when their character is sent to this game.
struct NotifyMagicState final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyMagicState;

    NotifyMagicState()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override
    {
        Serialization::WriteVarInt(aWriter, CharacterId);
        State.Serialize(aWriter);
    }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ServerMessage::DeserializeRaw(aReader);
        CharacterId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        State.Deserialize(aReader);
    }

    bool operator==(const NotifyMagicState& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && CharacterId == acRhs.CharacterId && State == acRhs.State; }

    //! The player's character, by server id.
    uint32_t CharacterId{};
    MagicState State{};
};
