#pragma once

#include "Message.h"

#include <Structs/MagicState.h>

//! What this game's player casts and wears as magic, sent when it changes.
struct RequestMagicState final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestMagicState;

    RequestMagicState()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override
    {
        Serialization::WriteVarInt(aWriter, CharacterId);
        State.Serialize(aWriter);
    }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ClientMessage::DeserializeRaw(aReader);
        CharacterId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        State.Deserialize(aReader);
    }

    bool operator==(const RequestMagicState& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && CharacterId == acRhs.CharacterId && State == acRhs.State; }

    //! The player's character, by server id.
    uint32_t CharacterId{};
    MagicState State{};
};
