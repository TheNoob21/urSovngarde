#pragma once

#include <Structs/GameId.h>
#include <DecodeGuard.h>
#include <TiltedCore/Serialization.hpp>

//! A lever's or a pillar's end state: its reference and its resting pose (ActivationCatchUp::Pose).
struct ActivationState
{
    //! ActivationCatchUp::kMaxPoseValues.
    static constexpr uint64_t kMaxPoseValues = 192;

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
    {
        Id.Serialize(aWriter);
        Serialization::WriteVarInt(aWriter, Pose.size());
        for (const int16_t cValue : Pose)
            aWriter.WriteBits(static_cast<uint16_t>(cValue), 16);
    }

    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
    {
        Id.Deserialize(aReader);
        const auto cCount = Serialization::ReadVarInt(aReader);
        if (!DecodeGuard::Within(cCount, kMaxPoseValues))
            return;
        Pose.resize(static_cast<size_t>(cCount));
        for (int16_t& value : Pose)
        {
            uint64_t bits{};
            if (!aReader.ReadBits(bits, 16))
            {
                DecodeGuard::Fail();
                return;
            }
            value = static_cast<int16_t>(static_cast<uint16_t>(bits));
        }
    }

    bool operator==(const ActivationState& acRhs) const noexcept { return Id == acRhs.Id && Pose == acRhs.Pose; }

    GameId Id{};
    TiltedPhoques::Vector<int16_t> Pose{};
};
