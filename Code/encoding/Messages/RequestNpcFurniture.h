#pragma once

#include "Message.h"

#include <Structs/NpcFurnitureWord.h>

//! Where the NPCs this game runs sit or sleep: when that changes, and every 2 s while they are seated.
struct RequestNpcFurniture final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestNpcFurniture;

    RequestNpcFurniture()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { NpcFurnitureWords::Write(aWriter, Words); }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ClientMessage::DeserializeRaw(aReader);
        NpcFurnitureWords::Read(aReader, Words);
    }

    bool operator==(const RequestNpcFurniture& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Words == acRhs.Words; }

    TiltedPhoques::Vector<NpcFurnitureWord> Words{};
};
