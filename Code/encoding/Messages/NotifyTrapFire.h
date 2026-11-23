#pragma once

#include "Message.h"

#include <Structs/GameId.h>

//! To the players near it: a trap another player's game set off, for theirs to fire too.
struct NotifyTrapFire final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyTrapFire;

    NotifyTrapFire()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override
    {
        Id.Serialize(aWriter);
        TriggerId.Serialize(aWriter);
    }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ServerMessage::DeserializeRaw(aReader);
        Id.Deserialize(aReader);
        TriggerId.Deserialize(aReader);
    }

    bool operator==(const NotifyTrapFire& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Id == acRhs.Id && TriggerId == acRhs.TriggerId; }

    //! The trap and the trigger that fired it.
    GameId Id{};
    GameId TriggerId{};
};
