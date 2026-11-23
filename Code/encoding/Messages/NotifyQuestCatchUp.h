#pragma once

#include "Message.h"

#include <Structs/QuestStep.h>

//! To a player joining a party: the quest starts, stages and stops relayed to it before, in the order they came.
struct NotifyQuestCatchUp final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyQuestCatchUp;

    NotifyQuestCatchUp()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { QuestSteps::Write(aWriter, Steps); }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ServerMessage::DeserializeRaw(aReader);
        QuestSteps::Read(aReader, Steps);
    }

    bool operator==(const NotifyQuestCatchUp& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Steps == acRhs.Steps; }

    TiltedPhoques::Vector<QuestStep> Steps{};
};
