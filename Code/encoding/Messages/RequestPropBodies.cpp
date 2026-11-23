#include <Messages/RequestPropBodies.h>

void RequestPropBodies::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    ObjectId.Serialize(aWriter);
    CellId.Serialize(aWriter);
    WorldSpaceId.Serialize(aWriter);
    Position.Serialize(aWriter);
    Bodies.Serialize(aWriter);
    Serialization::WriteBool(aWriter, AtRest);
    Serialization::WriteVarInt(aWriter, Tick);
}

void RequestPropBodies::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    ObjectId.Deserialize(aReader);
    CellId.Deserialize(aReader);
    WorldSpaceId.Deserialize(aReader);
    Position.Deserialize(aReader);
    Bodies.Deserialize(aReader);
    AtRest = Serialization::ReadBool(aReader);
    Tick = Serialization::ReadVarInt(aReader);
}
