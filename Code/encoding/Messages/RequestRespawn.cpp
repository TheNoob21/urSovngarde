#include <Messages/RequestRespawn.h>
#include <BoundedString.h>

void RequestRespawn::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ActorId);
    Serialization::WriteString(aWriter, AppearanceBuffer);
    Serialization::WriteVarInt(aWriter, ChangeFlags);
    FaceTints.Serialize(aWriter);
    Serialization::WriteString(aWriter, Username);
}

void RequestRespawn::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    ActorId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    AppearanceBuffer = BoundedString::Read(aReader, BoundedString::kLargest);
    ChangeFlags = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    FaceTints.Deserialize(aReader);
    Username = BoundedString::Read(aReader, 256);
}
