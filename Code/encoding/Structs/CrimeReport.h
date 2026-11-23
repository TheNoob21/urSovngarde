#pragma once

#include <Structs/GameId.h>
#include <DecodeGuard.h>

#include <TiltedCore/Serialization.hpp>

//! A crime one player's game saw its player commit, for the games that run its witnesses (CrimeService).
struct CrimeReport
{
    //! CrimeReports::Kind, the game's own crime numbers: 0 theft, 1 pickpocket, 2 trespass, 3 assault, 4 murder.
    uint8_t Kind{};
    //! The server id of the robbed or attacked actor; 0 for a theft from a shelf or a chest.
    uint32_t VictimId{};
    //! The reference stolen from (the item or its container); empty for a temporary or gone one.
    GameId Ref{};
    //! The stolen item's base form.
    GameId Object{};
    //! Whom the game's crime alarm gave it to: an NPC's base or a faction.
    GameId Owner{};
    uint32_t Count{};
    //! Its value in gold, as the alarm had it.
    uint32_t Value{};
    //! The server ids of the NPC copies that saw it (DetectionService's word for them was "seen").
    TiltedPhoques::Vector<uint32_t> Witnesses{};

    bool operator==(const CrimeReport& acRhs) const noexcept
    {
        return Kind == acRhs.Kind && VictimId == acRhs.VictimId && Ref == acRhs.Ref && Object == acRhs.Object && Owner == acRhs.Owner && Count == acRhs.Count &&
               Value == acRhs.Value && Witnesses == acRhs.Witnesses;
    }
};

namespace CrimeReportLimits
{
constexpr uint8_t kKindLimit = 5;
constexpr uint64_t kMaxWitnesses = 16;
constexpr uint64_t kMaxCount = 100000;
constexpr uint64_t kMaxValue = 100000000;

inline void Write(TiltedPhoques::Buffer::Writer& aWriter, const CrimeReport& acReport) noexcept
{
    aWriter.WriteBits(acReport.Kind, 3);
    Serialization::WriteVarInt(aWriter, acReport.VictimId);
    acReport.Ref.Serialize(aWriter);
    acReport.Object.Serialize(aWriter);
    acReport.Owner.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, acReport.Count);
    Serialization::WriteVarInt(aWriter, acReport.Value);
    Serialization::WriteVarInt(aWriter, acReport.Witnesses.size());
    for (const uint32_t cWitness : acReport.Witnesses)
        Serialization::WriteVarInt(aWriter, cWitness);
}

inline void Read(TiltedPhoques::Buffer::Reader& aReader, CrimeReport& aReport) noexcept
{
    uint64_t kind = 0;
    aReader.ReadBits(kind, 3);
    if (kind >= kKindLimit)
        DecodeGuard::Fail();
    aReport.Kind = kind & 0x7;
    aReport.VictimId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    aReport.Ref.Deserialize(aReader);
    aReport.Object.Deserialize(aReader);
    aReport.Owner.Deserialize(aReader);
    const uint64_t cCount = Serialization::ReadVarInt(aReader);
    const uint64_t cValue = Serialization::ReadVarInt(aReader);
    if (!DecodeGuard::Within(cCount, kMaxCount) || !DecodeGuard::Within(cValue, kMaxValue))
        return;
    aReport.Count = static_cast<uint32_t>(cCount);
    aReport.Value = static_cast<uint32_t>(cValue);
    const uint64_t cWitnesses = Serialization::ReadVarInt(aReader);
    if (!DecodeGuard::Within(cWitnesses, kMaxWitnesses))
        return;
    aReport.Witnesses.resize(static_cast<size_t>(cWitnesses));
    for (auto& witness : aReport.Witnesses)
        witness = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}
} // namespace CrimeReportLimits
