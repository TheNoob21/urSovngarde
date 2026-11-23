#pragma once

#include "Message.h"

#include <Structs/QuestAliases.h>

//! The quest alias fills of the game that picked them, sent when they are new or changed (QuestAliasSync, client).
struct RequestQuestAliases final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestQuestAliases;

    RequestQuestAliases()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { Aliases.Write(aWriter); }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ClientMessage::DeserializeRaw(aReader);
        Aliases.Read(aReader);
    }

    bool operator==(const RequestQuestAliases& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Aliases == acRhs.Aliases; }

    QuestAliases Aliases{};
};
