#include <Messages/DialogueRequest.h>
#include <BoundedString.h>

void DialogueRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    Serialization::WriteString(aWriter, SoundFilename);
    SceneId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, ScenePhase);
}

void DialogueRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    SoundFilename = BoundedString::Read(aReader, 512);
    SceneId.Deserialize(aReader);
    ScenePhase = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}
