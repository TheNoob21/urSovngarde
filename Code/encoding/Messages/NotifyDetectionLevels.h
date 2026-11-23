#pragma once

#include "Message.h"

#include <Structs/DetectionWord.h>

//! To one player: whether the NPCs another game runs see this player's character, for this game's copies of them.
struct NotifyDetectionLevels final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyDetectionLevels;

    NotifyDetectionLevels()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { DetectionWords::Write(aWriter, Words); }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ServerMessage::DeserializeRaw(aReader);
        DetectionWords::Read(aReader, Words);
    }

    bool operator==(const NotifyDetectionLevels& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Words == acRhs.Words; }

    TiltedPhoques::Vector<DetectionWord> Words{};
};
