#pragma once

#include "Message.h"

#include <Structs/CombatTargetWord.h>

//! To the players near them: whom the NPCs another game runs fight, for this game's copies of them.
struct NotifyCombatTarget final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyCombatTarget;

    NotifyCombatTarget()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { CombatTargetWords::Write(aWriter, Words); }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ServerMessage::DeserializeRaw(aReader);
        CombatTargetWords::Read(aReader, Words);
    }

    bool operator==(const NotifyCombatTarget& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Words == acRhs.Words; }

    TiltedPhoques::Vector<CombatTargetWord> Words{};
};
