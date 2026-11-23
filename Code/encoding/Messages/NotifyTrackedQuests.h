#pragma once

#include "Message.h"

#include <Structs/TrackedQuest.h>

//! To the party's members: the quests their leader's journal follows, for theirs to follow.
struct NotifyTrackedQuests final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyTrackedQuests;

    NotifyTrackedQuests()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { TrackedQuests::Write(aWriter, Quests); }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ServerMessage::DeserializeRaw(aReader);
        TrackedQuests::Read(aReader, Quests);
    }

    bool operator==(const NotifyTrackedQuests& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Quests == acRhs.Quests; }

    TiltedPhoques::Vector<TrackedQuest> Quests{};
};
