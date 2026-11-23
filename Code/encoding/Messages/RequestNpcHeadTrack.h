#pragma once

#include "Message.h"

#include <Structs/NpcHeadTrackWord.h>

//! Where the NPCs this game runs look: when that changes, and every second while they look at something.
struct RequestNpcHeadTrack final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestNpcHeadTrack;

    RequestNpcHeadTrack()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { NpcHeadTrackWords::Write(aWriter, Words); }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ClientMessage::DeserializeRaw(aReader);
        NpcHeadTrackWords::Read(aReader, Words);
    }

    bool operator==(const RequestNpcHeadTrack& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Words == acRhs.Words; }

    TiltedPhoques::Vector<NpcHeadTrackWord> Words{};
};
