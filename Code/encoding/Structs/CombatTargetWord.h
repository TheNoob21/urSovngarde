#pragma once

#include <DecodeGuard.h>

#include <TiltedCore/Serialization.hpp>

//! The owner's word about one NPC it runs: whom it fights.
struct CombatTargetWord
{
    //! The NPC's server id.
    uint32_t NpcId{};
    //! The server id of whom it fights; 0 for nobody.
    uint32_t TargetId{};
    //! The sender's ownership epoch for the NPC; 0 is never valid.
    uint32_t OwnershipEpoch{};

    bool operator==(const CombatTargetWord& acRhs) const noexcept { return NpcId == acRhs.NpcId && TargetId == acRhs.TargetId && OwnershipEpoch == acRhs.OwnershipEpoch; }
};

namespace CombatTargetWords
{
//! A town's worth of fights, with room to spare.
constexpr uint64_t kMax = 512;

inline void Write(TiltedPhoques::Buffer::Writer& aWriter, const TiltedPhoques::Vector<CombatTargetWord>& acWords) noexcept
{
    Serialization::WriteVarInt(aWriter, acWords.size());
    for (const auto& word : acWords)
    {
        Serialization::WriteVarInt(aWriter, word.NpcId);
        Serialization::WriteVarInt(aWriter, word.TargetId);
        Serialization::WriteVarInt(aWriter, word.OwnershipEpoch);
    }
}

inline void Read(TiltedPhoques::Buffer::Reader& aReader, TiltedPhoques::Vector<CombatTargetWord>& aWords) noexcept
{
    const auto cCount = Serialization::ReadVarInt(aReader);
    if (!DecodeGuard::Within(cCount, kMax))
        return;
    aWords.resize(static_cast<size_t>(cCount));
    for (auto& word : aWords)
    {
        word.NpcId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        word.TargetId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        word.OwnershipEpoch = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    }
}
} // namespace CombatTargetWords
