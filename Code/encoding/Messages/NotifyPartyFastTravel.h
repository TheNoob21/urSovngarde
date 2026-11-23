#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <TiltedCore/Serialization.hpp>

//! To the party's members away from the leader: the leader fast-travelled there, and they may follow for a while.
struct NotifyPartyFastTravel final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPartyFastTravel;

    NotifyPartyFastTravel()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override
    {
        Serialization::WriteVarInt(aWriter, LeaderId);
        CellId.Serialize(aWriter);
        WorldSpaceId.Serialize(aWriter);
    }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ServerMessage::DeserializeRaw(aReader);
        LeaderId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        CellId.Deserialize(aReader);
        WorldSpaceId.Deserialize(aReader);
    }

    bool operator==(const NotifyPartyFastTravel& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && LeaderId == acRhs.LeaderId && CellId == acRhs.CellId && WorldSpaceId == acRhs.WorldSpaceId;
    }

    uint32_t LeaderId{};
    GameId CellId{};
    GameId WorldSpaceId{};
};
