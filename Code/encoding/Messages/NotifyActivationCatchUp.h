#pragma once

#include "Message.h"

#include <Structs/ActivationState.h>
#include <Structs/GameId.h>
#include <DecodeGuard.h>

//! Activations of references near this player that other games made while this one was not there, oldest first.
//! The receiver replays only the mechanisms among them (levers, chains, puzzle pillars, claw rings; ActivationCatchUp.h).
//! A lever or a pillar whose end state is known comes as that state instead, and is set, not replayed.
struct NotifyActivationCatchUp final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyActivationCatchUp;

    //! The server sends at most this many at once (ActivationLedger::kMaxCatchUp).
    static constexpr uint64_t kMax = 256;
    //! At most this many end states at once (ActivationLedger::kMaxStateCatchUp).
    static constexpr uint64_t kMaxStates = 64;

    NotifyActivationCatchUp()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override
    {
        Serialization::WriteVarInt(aWriter, Activations.size());
        for (const auto& id : Activations)
            id.Serialize(aWriter);
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
        Activations.resize(static_cast<size_t>(cCount));
        for (auto& id : Activations)
            id.Deserialize(aReader);
        const auto cStateCount = Serialization::ReadVarInt(aReader);
        if (!DecodeGuard::Within(cStateCount, kMaxStates))
            return;
        States.resize(static_cast<size_t>(cStateCount));
        for (auto& state : States)
            state.Deserialize(aReader);
    }

    bool operator==(const NotifyActivationCatchUp& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Activations == acRhs.Activations && States == acRhs.States; }

    TiltedPhoques::Vector<GameId> Activations{};
    TiltedPhoques::Vector<ActivationState> States{};
};
