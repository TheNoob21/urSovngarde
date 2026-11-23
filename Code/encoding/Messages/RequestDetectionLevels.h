#pragma once

#include "Message.h"

#include <Structs/DetectionWord.h>

//! Whether the NPCs this game runs see the other players' characters near them: when that changes, and every 2 s.
struct RequestDetectionLevels final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestDetectionLevels;

    RequestDetectionLevels()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { DetectionWords::Write(aWriter, Words); }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ClientMessage::DeserializeRaw(aReader);
        DetectionWords::Read(aReader, Words);
    }

    bool operator==(const RequestDetectionLevels& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Words == acRhs.Words; }

    TiltedPhoques::Vector<DetectionWord> Words{};
};
