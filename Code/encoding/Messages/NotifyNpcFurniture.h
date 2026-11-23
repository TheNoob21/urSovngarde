#pragma once

#include "Message.h"

#include <Structs/NpcFurnitureWord.h>

//! To the players near them: where the NPCs another game runs sit or sleep, for this game's copies of them.
struct NotifyNpcFurniture final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyNpcFurniture;

    NotifyNpcFurniture()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { NpcFurnitureWords::Write(aWriter, Words); }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ServerMessage::DeserializeRaw(aReader);
        NpcFurnitureWords::Read(aReader, Words);
    }

    bool operator==(const NotifyNpcFurniture& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Words == acRhs.Words; }

    TiltedPhoques::Vector<NpcFurnitureWord> Words{};
};
