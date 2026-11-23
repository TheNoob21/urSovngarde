#include <Messages/RequestHitReaction.h>

void RequestHitReaction::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Id);
    aWriter.WriteBits(Kind, 8);
    Serialization::WriteFloat(aWriter, Magnitude);
    Serialization::WriteFloat(aWriter, Direction);
}

void RequestHitReaction::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Id = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    uint64_t kind = 0;
    aReader.ReadBits(kind, 8);
    Kind = static_cast<uint8_t>(kind);
    Magnitude = Serialization::ReadFloat(aReader);
    Direction = Serialization::ReadFloat(aReader);
}
