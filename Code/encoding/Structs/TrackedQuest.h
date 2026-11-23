#pragma once

#include <Structs/GameId.h>
#include <DecodeGuard.h>

#include <TiltedCore/Serialization.hpp>

//! A quest the party leader's journal follows (tracks): the one whose markers its compass and map show.
struct TrackedQuest
{
    GameId Id{};
    //! As in RequestQuestUpdate, for the server's misc quest setting.
    uint8_t ClientQuestType{};

    bool operator==(const TrackedQuest& acRhs) const noexcept { return Id == acRhs.Id && ClientQuestType == acRhs.ClientQuestType; }
};

namespace TrackedQuests
{
//! A journal follows a handful at most; more than this is no one's choice.
constexpr uint64_t kMax = 128;

inline void Write(TiltedPhoques::Buffer::Writer& aWriter, const TiltedPhoques::Vector<TrackedQuest>& acQuests) noexcept
{
    Serialization::WriteVarInt(aWriter, acQuests.size());
    for (const auto& quest : acQuests)
    {
        quest.Id.Serialize(aWriter);
        aWriter.WriteBits(quest.ClientQuestType, 8);
    }
}

inline void Read(TiltedPhoques::Buffer::Reader& aReader, TiltedPhoques::Vector<TrackedQuest>& aQuests) noexcept
{
    const auto cCount = Serialization::ReadVarInt(aReader);
    if (!DecodeGuard::Within(cCount, kMax))
        return;
    aQuests.resize(static_cast<size_t>(cCount));
    for (auto& quest : aQuests)
    {
        quest.Id.Deserialize(aReader);
        uint64_t tmp = 0;
        aReader.ReadBits(tmp, 8);
        quest.ClientQuestType = tmp & 0xFF;
    }
}
} // namespace TrackedQuests
