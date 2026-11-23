#include <Messages/NotifyFollowerCommand.h>

void NotifyFollowerCommand::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    aWriter.WriteBits(Command, 8);
    aWriter.WriteBits(Reply, 8);
    Serialization::WriteVarInt(aWriter, FromPlayerId);
}

void NotifyFollowerCommand::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Command = static_cast<uint8_t>(value);
    aReader.ReadBits(value, 8);
    Reply = static_cast<uint8_t>(value);
    FromPlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}
