#include <Structs/RagdollPose.h>
#include <DecodeGuard.h>
#include <TiltedCore/Serialization.hpp>

#include <algorithm>
#include <cstring>

using TiltedPhoques::Serialization;

bool RagdollPose::operator==(const RagdollPose& acRhs) const noexcept
{
    return Tick == acRhs.Tick && Layout == acRhs.Layout && BodyCount == acRhs.BodyCount && std::memcmp(Root, acRhs.Root, sizeof(Root)) == 0 && Bodies == acRhs.Bodies;
}

void RagdollPose::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Tick);
    aWriter.WriteBits(Layout, 32);
    aWriter.WriteBits(BodyCount, 8);
    // Raw: world coordinates reach six figures, too large to quantise without moving the body.
    for (const float cValue : Root)
        Serialization::WriteFloat(aWriter, cValue);
    const size_t cCount = std::min(Bodies.size(), kMaxBodies);
    aWriter.WriteBits(cCount, 6);
    for (size_t i = 0; i < cCount; ++i)
    {
        for (const int16_t cValue : Bodies[i].Offset)
            aWriter.WriteBits(static_cast<uint16_t>(cValue), 16);
        Bodies[i].Rotation.Serialize(aWriter);
    }
}

void RagdollPose::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Bodies.clear();
    Tick = Serialization::ReadVarInt(aReader);
    uint64_t bits = 0;
    aReader.ReadBits(bits, 32);
    Layout = static_cast<uint32_t>(bits);
    bits = 0;
    aReader.ReadBits(bits, 8);
    BodyCount = static_cast<uint8_t>(bits);
    for (float& value : Root)
        value = Serialization::ReadFloat(aReader);
    bits = 0;
    aReader.ReadBits(bits, 6);
    if (!DecodeGuard::Within(bits, std::min<uint64_t>(kMaxBodies, BodyCount)))
        return;
    Bodies.resize(static_cast<size_t>(bits));
    for (Body& body : Bodies)
    {
        for (int16_t& value : body.Offset)
        {
            bits = 0;
            aReader.ReadBits(bits, 16);
            value = static_cast<int16_t>(static_cast<uint16_t>(bits));
        }
        body.Rotation.Deserialize(aReader);
    }
}
