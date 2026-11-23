#pragma once

#include "Message.h"

#include <Structs/ReferenceState.h>
#include <DecodeGuard.h>

//! World reference states other players' games changed (ReferenceStates.h): live to the players near the reference, and to a
//! player who comes near later the kept ones, oldest first, with their idle hours.
struct NotifyReferenceStates final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyReferenceStates;

    //! The server sends at most this many at once (ReferenceStateLedger::kMaxCatchUp).
    static constexpr uint64_t kMax = 256;

    NotifyReferenceStates()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override
    {
        Serialization::WriteVarInt(aWriter, States.size());
        for (const auto& state : States)
            state.Serialize(aWriter);
    }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ServerMessage::DeserializeRaw(aReader);
        const auto cCount = Serialization::ReadVarInt(aReader);
        if (!DecodeGuard::Within(cCount, kMax))
            return;
        States.resize(static_cast<size_t>(cCount));
        for (auto& state : States)
            state.Deserialize(aReader);
    }

    bool operator==(const NotifyReferenceStates& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && States == acRhs.States; }

    TiltedPhoques::Vector<ReferenceState> States{};
};
