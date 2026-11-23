#pragma once

#include <DecodeGuard.h>

#include <TiltedCore/Serialization.hpp>

#include <cmath>
#include <limits>

//! The owner's word about where one NPC it runs looks: its animation graph's head-track flags and target point.
struct NpcHeadTrackWord
{
    //! The NPC's server id.
    uint32_t NpcId{};
    //! bHeadTracking (1) and bHeadTrackSpine (2); 0 when it no longer looks at anything.
    uint8_t Flags{};
    //! The graph's TargetLocation, sent to a quarter of a unit.
    float X{};
    float Y{};
    float Z{};
    //! The sender's ownership epoch for the NPC; 0 is never valid.
    uint32_t OwnershipEpoch{};

    bool operator==(const NpcHeadTrackWord& acRhs) const noexcept
    {
        return NpcId == acRhs.NpcId && Flags == acRhs.Flags && X == acRhs.X && Y == acRhs.Y && Z == acRhs.Z && OwnershipEpoch == acRhs.OwnershipEpoch;
    }
};

namespace NpcHeadTrackWords
{
//! A town's worth of NPCs, with room to spare.
constexpr uint64_t kMax = 512;

//! A coordinate in quarter units, zigzagged so that small negative numbers stay short. Out of range or not a number is 0.
inline uint64_t PackCoordinate(const float aValue) noexcept
{
    constexpr float cLimit = static_cast<float>(std::numeric_limits<int32_t>::max() / 8);
    const int64_t cQuarters = std::isfinite(aValue) && std::fabs(aValue) < cLimit ? static_cast<int64_t>(std::llround(aValue * 4.f)) : 0;
    return (static_cast<uint64_t>(cQuarters) << 1) ^ static_cast<uint64_t>(cQuarters >> 63);
}

inline float UnpackCoordinate(const uint64_t aValue) noexcept
{
    const int64_t cQuarters = static_cast<int64_t>(aValue >> 1) ^ -static_cast<int64_t>(aValue & 1);
    return static_cast<float>(static_cast<int32_t>(cQuarters)) / 4.f;
}

inline void Write(TiltedPhoques::Buffer::Writer& aWriter, const TiltedPhoques::Vector<NpcHeadTrackWord>& acWords) noexcept
{
    Serialization::WriteVarInt(aWriter, acWords.size());
    for (const auto& word : acWords)
    {
        Serialization::WriteVarInt(aWriter, word.NpcId);
        Serialization::WriteVarInt(aWriter, word.Flags);
        Serialization::WriteVarInt(aWriter, PackCoordinate(word.X));
        Serialization::WriteVarInt(aWriter, PackCoordinate(word.Y));
        Serialization::WriteVarInt(aWriter, PackCoordinate(word.Z));
        Serialization::WriteVarInt(aWriter, word.OwnershipEpoch);
    }
}

inline void Read(TiltedPhoques::Buffer::Reader& aReader, TiltedPhoques::Vector<NpcHeadTrackWord>& aWords) noexcept
{
    const auto cCount = Serialization::ReadVarInt(aReader);
    if (!DecodeGuard::Within(cCount, kMax))
        return;
    aWords.resize(static_cast<size_t>(cCount));
    for (auto& word : aWords)
    {
        word.NpcId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        word.Flags = static_cast<uint8_t>(Serialization::ReadVarInt(aReader) & 0xFF);
        word.X = UnpackCoordinate(Serialization::ReadVarInt(aReader));
        word.Y = UnpackCoordinate(Serialization::ReadVarInt(aReader));
        word.Z = UnpackCoordinate(Serialization::ReadVarInt(aReader));
        word.OwnershipEpoch = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    }
}
} // namespace NpcHeadTrackWords
