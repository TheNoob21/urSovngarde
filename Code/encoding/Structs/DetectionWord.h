#pragma once

#include <DecodeGuard.h>

#include <TiltedCore/Serialization.hpp>

//! The owner's word about one NPC it runs and one other player: whether the NPC sees that player's character.
struct DetectionWord
{
    //! The NPC's server id.
    uint32_t NpcId{};
    //! The player it does or does not see.
    uint32_t PlayerId{};
    //! DetectionLevels::Band: unseen, noticed or seen.
    uint8_t Band{};

    bool operator==(const DetectionWord& acRhs) const noexcept { return NpcId == acRhs.NpcId && PlayerId == acRhs.PlayerId && Band == acRhs.Band; }
};

namespace DetectionWords
{
//! A town of NPCs near a few players, with room to spare.
constexpr uint64_t kMax = 512;

inline void Write(TiltedPhoques::Buffer::Writer& aWriter, const TiltedPhoques::Vector<DetectionWord>& acWords) noexcept
{
    Serialization::WriteVarInt(aWriter, acWords.size());
    for (const auto& word : acWords)
    {
        Serialization::WriteVarInt(aWriter, word.NpcId);
        Serialization::WriteVarInt(aWriter, word.PlayerId);
        aWriter.WriteBits(word.Band, 2);
    }
}

inline void Read(TiltedPhoques::Buffer::Reader& aReader, TiltedPhoques::Vector<DetectionWord>& aWords) noexcept
{
    const auto cCount = Serialization::ReadVarInt(aReader);
    if (!DecodeGuard::Within(cCount, kMax))
        return;
    aWords.resize(static_cast<size_t>(cCount));
    for (auto& word : aWords)
    {
        word.NpcId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        word.PlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        uint64_t tmp = 0;
        aReader.ReadBits(tmp, 2);
        word.Band = tmp & 0x3;
    }
}
} // namespace DetectionWords
