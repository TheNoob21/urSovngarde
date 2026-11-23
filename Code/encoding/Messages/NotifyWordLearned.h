#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <DecodeGuard.h>

//! To the party's other members: words of power one of them was taught, for theirs to learn; every word the party was taught,
//! for a member who joins later. Unlocking a word stays each player's.
struct NotifyWordLearned final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyWordLearned;

    //! The server sends at most this many at once (PartyWords::kMaxPerParty).
    static constexpr uint64_t kMax = 512;

    NotifyWordLearned()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override
    {
        Serialization::WriteVarInt(aWriter, Words.size());
        for (const auto& id : Words)
            id.Serialize(aWriter);
    }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ServerMessage::DeserializeRaw(aReader);
        const auto cCount = Serialization::ReadVarInt(aReader);
        if (!DecodeGuard::Within(cCount, kMax))
            return;
        Words.resize(static_cast<size_t>(cCount));
        for (auto& id : Words)
            id.Deserialize(aReader);
    }

    bool operator==(const NotifyWordLearned& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Words == acRhs.Words; }

    TiltedPhoques::Vector<GameId> Words{};
};
