#pragma once

#include "Message.h"

#include <Structs/CombatTargetWord.h>

//! Whom the NPCs this game runs fight: when that changes, and every 2 s while they fight.
struct RequestCombatTarget final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestCombatTarget;

    RequestCombatTarget()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { CombatTargetWords::Write(aWriter, Words); }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ClientMessage::DeserializeRaw(aReader);
        CombatTargetWords::Read(aReader, Words);
    }

    bool operator==(const RequestCombatTarget& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Words == acRhs.Words; }

    TiltedPhoques::Vector<CombatTargetWord> Words{};
};
