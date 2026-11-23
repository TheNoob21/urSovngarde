#include <Structs/PropBodyState.h>
#include <Structs/Quaternion_NetQuantize.h>
#include <DecodeGuard.h>
#include <TiltedCore/Serialization.hpp>

#include <algorithm>
#include <cmath>

using TiltedPhoques::Serialization;

namespace
{
constexpr float kUnitsPerStep = 1.f / 16.f;

int16_t Quantize(const float aValue) noexcept
{
    const float cClamped = std::isfinite(aValue) ? std::clamp(aValue, -PropBodyState::kMaxDistance, PropBodyState::kMaxDistance) : 0.f;
    return static_cast<int16_t>(std::lround(cClamped / kUnitsPerStep));
}

uint32_t PackRotation(const glm::quat& acRotation) noexcept
{
    Quaternion_NetQuantize rotation{};
    rotation = acRotation;
    return rotation.Pack();
}
} // namespace

bool PropBodyPose::operator==(const PropBodyPose& acRhs) const noexcept
{
    for (int axis = 0; axis < 3; ++axis)
        if (Quantize(Position[axis]) != Quantize(acRhs.Position[axis]))
            return false;
    return PackRotation(Rotation) == PackRotation(acRhs.Rotation);
}

void PropBodyState::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Layout, 32);
    const size_t cCount = std::min(Bodies.size(), kMaxBodies);
    Serialization::WriteVarInt(aWriter, cCount);
    for (size_t i = 0; i < cCount; ++i)
    {
        for (int axis = 0; axis < 3; ++axis)
            aWriter.WriteBits(static_cast<uint16_t>(Quantize(Bodies[i].Position[axis])), 16);
        aWriter.WriteBits(PackRotation(Bodies[i].Rotation), 32);
    }
}

void PropBodyState::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Bodies.clear();
    uint64_t layout = 0;
    aReader.ReadBits(layout, 32);
    Layout = static_cast<uint32_t>(layout);
    const uint64_t cCount = Serialization::ReadVarInt(aReader);
    if (!DecodeGuard::Within(cCount, kMaxBodies))
        return;
    for (uint64_t i = 0; i < cCount; ++i)
    {
        PropBodyPose body{};
        for (int axis = 0; axis < 3; ++axis)
        {
            uint64_t bits = 0;
            aReader.ReadBits(bits, 16);
            body.Position[axis] = static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(bits))) * kUnitsPerStep;
        }
        uint64_t rotation = 0;
        aReader.ReadBits(rotation, 32);
        Quaternion_NetQuantize unpacked{};
        unpacked.Unpack(static_cast<uint32_t>(rotation));
        body.Rotation = unpacked;
        Bodies.push_back(body);
    }
}
