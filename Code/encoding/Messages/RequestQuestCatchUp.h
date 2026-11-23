#pragma once

#include "Message.h"

//! A member's game asks again for the quest steps its party was sent, after it loaded a save while connected.
//! The server answers with a NotifyQuestCatchUp, as it does when a player joins (QuestHistory::MaySendCatchUp).
struct RequestQuestCatchUp final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestQuestCatchUp;

    RequestQuestCatchUp()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer&) const noexcept override {}

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override { ClientMessage::DeserializeRaw(aReader); }

    bool operator==(const RequestQuestCatchUp& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode(); }
};
