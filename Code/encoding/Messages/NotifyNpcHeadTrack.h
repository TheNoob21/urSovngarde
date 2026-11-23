#pragma once

#include "Message.h"

#include <Structs/NpcHeadTrackWord.h>

//! To the players near them: where the NPCs another game runs look, for this game's copies of them.
struct NotifyNpcHeadTrack final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyNpcHeadTrack;

    NotifyNpcHeadTrack()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { NpcHeadTrackWords::Write(aWriter, Words); }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ServerMessage::DeserializeRaw(aReader);
        NpcHeadTrackWords::Read(aReader, Words);
    }

    bool operator==(const NotifyNpcHeadTrack& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Words == acRhs.Words; }

    TiltedPhoques::Vector<NpcHeadTrackWord> Words{};
};
