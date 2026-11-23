#pragma once

#include <DecodeGuard.h>
#include <Structs/RuntimeSeat.h>

#include <TiltedCore/Serialization.hpp>

//! The owner's word about one NPC it runs: the furniture it is settled in, or none.
struct NpcFurnitureWord
{
    //! The NPC's server id.
    uint32_t NpcId{};
    //! The furniture reference, by the plugin that placed it (its base furniture form for one placed at run time); 0 when not seated.
    uint32_t FurnitureModId{};
    uint32_t FurnitureBaseId{};
    //! The furniture marker it uses.
    uint32_t Marker{};
    //! The game's sit/sleep state: 3 sitting, 7 sleeping, 0 not seated.
    uint8_t State{};
    //! The sender's ownership epoch for the NPC; 0 is never valid.
    uint32_t OwnershipEpoch{};
    //! Where furniture placed at run time stands (RuntimeSeat.h).
    RuntimeSeat Placement{};

    bool operator==(const NpcFurnitureWord& acRhs) const noexcept
    {
        return NpcId == acRhs.NpcId && FurnitureModId == acRhs.FurnitureModId && FurnitureBaseId == acRhs.FurnitureBaseId && Marker == acRhs.Marker && State == acRhs.State &&
               OwnershipEpoch == acRhs.OwnershipEpoch && Placement == acRhs.Placement;
    }
};

namespace NpcFurnitureWords
{
//! A town's worth of seated NPCs, with room to spare.
constexpr uint64_t kMax = 512;

inline void Write(TiltedPhoques::Buffer::Writer& aWriter, const TiltedPhoques::Vector<NpcFurnitureWord>& acWords) noexcept
{
    Serialization::WriteVarInt(aWriter, acWords.size());
    for (const auto& word : acWords)
    {
        Serialization::WriteVarInt(aWriter, word.NpcId);
        Serialization::WriteVarInt(aWriter, word.State);
        Serialization::WriteVarInt(aWriter, word.FurnitureModId);
        Serialization::WriteVarInt(aWriter, word.FurnitureBaseId);
        Serialization::WriteVarInt(aWriter, word.Marker);
        Serialization::WriteVarInt(aWriter, word.OwnershipEpoch);
        RuntimeSeats::Write(aWriter, word.Placement);
    }
}

inline void Read(TiltedPhoques::Buffer::Reader& aReader, TiltedPhoques::Vector<NpcFurnitureWord>& aWords) noexcept
{
    const auto cCount = Serialization::ReadVarInt(aReader);
    if (!DecodeGuard::Within(cCount, kMax))
        return;
    aWords.resize(static_cast<size_t>(cCount));
    for (auto& word : aWords)
    {
        word.NpcId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        word.State = static_cast<uint8_t>(Serialization::ReadVarInt(aReader) & 0xFF);
        word.FurnitureModId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        word.FurnitureBaseId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        word.Marker = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        word.OwnershipEpoch = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        word.Placement = RuntimeSeats::Read(aReader);
    }
}
} // namespace NpcFurnitureWords
