#pragma once

#include <Structs/GameId.h>
#include <DecodeGuard.h>

#include <TiltedCore/Serialization.hpp>

//! A quest start, stage or stop as it was relayed to a party (NotifyQuestUpdate's fields).
struct QuestStep
{
    GameId Id{};
    uint16_t Stage{};
    uint8_t Status{};
    uint8_t ClientQuestType{};

    bool operator==(const QuestStep& acRhs) const noexcept { return Id == acRhs.Id && Stage == acRhs.Stage && Status == acRhs.Status && ClientQuestType == acRhs.ClientQuestType; }
};

namespace QuestSteps
{
//! What a party keeps for a player who joins it later (QuestHistory::kMax), with room to spare.
constexpr uint64_t kMax = 4096;

inline void Write(TiltedPhoques::Buffer::Writer& aWriter, const TiltedPhoques::Vector<QuestStep>& acSteps) noexcept
{
    Serialization::WriteVarInt(aWriter, acSteps.size());
    for (const auto& step : acSteps)
    {
        step.Id.Serialize(aWriter);
        aWriter.WriteBits(step.Stage, 16);
        aWriter.WriteBits(step.Status, 8);
        aWriter.WriteBits(step.ClientQuestType, 8);
    }
}

inline void Read(TiltedPhoques::Buffer::Reader& aReader, TiltedPhoques::Vector<QuestStep>& aSteps) noexcept
{
    const auto cCount = Serialization::ReadVarInt(aReader);
    if (!DecodeGuard::Within(cCount, kMax))
        return;
    aSteps.resize(static_cast<size_t>(cCount));
    for (auto& step : aSteps)
    {
        step.Id.Deserialize(aReader);
        uint64_t tmp = 0;
        aReader.ReadBits(tmp, 16);
        step.Stage = tmp & 0xFFFF;
        aReader.ReadBits(tmp, 8);
        step.Status = tmp & 0xFF;
        aReader.ReadBits(tmp, 8);
        step.ClientQuestType = tmp & 0xFF;
    }
}
} // namespace QuestSteps
