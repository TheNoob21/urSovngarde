#pragma once

#include "Message.h"

#include <Structs/GameId.h>

//! A word of power this player was taught, by a word wall or a quest, for the party to learn too.
struct RequestWordLearned final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestWordLearned;

    RequestWordLearned()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { WordId.Serialize(aWriter); }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ClientMessage::DeserializeRaw(aReader);
        WordId.Deserialize(aReader);
    }

    bool operator==(const RequestWordLearned& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && WordId == acRhs.WordId; }

    GameId WordId{};
};
