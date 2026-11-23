#include <Messages/NotifyDismember.h>

void NotifyDismember::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    aWriter.WriteBits(Limbs, 16);
    aWriter.WriteBits(LastHitLimb, 8);
}

void NotifyDismember::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    uint64_t value = 0;
    aReader.ReadBits(value, 16);
    Limbs = static_cast<uint16_t>(value);
    aReader.ReadBits(value, 8);
    LastHitLimb = static_cast<uint8_t>(value);
}
