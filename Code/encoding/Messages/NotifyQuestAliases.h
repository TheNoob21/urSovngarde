#pragma once

#include "Message.h"

#include <Structs/QuestAliases.h>

//! Another party member's quest alias fills, from the game the server holds as that quest's decider (QuestAliasService, server).
struct NotifyQuestAliases final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyQuestAliases;

    NotifyQuestAliases()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { Aliases.Write(aWriter); }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ServerMessage::DeserializeRaw(aReader);
        Aliases.Read(aReader);
    }

    bool operator==(const NotifyQuestAliases& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Aliases == acRhs.Aliases; }

    QuestAliases Aliases{};
};
