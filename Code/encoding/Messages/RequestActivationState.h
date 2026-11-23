#pragma once

#include "Message.h"

#include <Structs/ActivationState.h>

//! The end state a lever or a pillar this player used came to rest in, for the games that come near it later.
struct RequestActivationState final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestActivationState;

    RequestActivationState()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override
    {
        CellId.Serialize(aWriter);
        State.Serialize(aWriter);
    }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ClientMessage::DeserializeRaw(aReader);
        CellId.Deserialize(aReader);
        State.Deserialize(aReader);
    }

    bool operator==(const RequestActivationState& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && CellId == acRhs.CellId && State == acRhs.State; }

    GameId CellId{};
    ActivationState State{};
};
