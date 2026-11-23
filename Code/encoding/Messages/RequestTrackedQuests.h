#pragma once

#include "Message.h"

#include <Structs/TrackedQuest.h>

//! The quests the party leader's journal follows: when they change, and every so often for whoever joined since.
struct RequestTrackedQuests final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestTrackedQuests;

    RequestTrackedQuests()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { TrackedQuests::Write(aWriter, Quests); }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ClientMessage::DeserializeRaw(aReader);
        TrackedQuests::Read(aReader, Quests);
    }

    bool operator==(const RequestTrackedQuests& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Quests == acRhs.Quests; }

    TiltedPhoques::Vector<TrackedQuest> Quests{};
};
