#include <Messages/NotifyDroppedItemMove.h>

void NotifyDroppedItemMove::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Id);
    Position.Serialize(aWriter);
    Serialization::WriteFloat(aWriter, Rotation.x);
    Serialization::WriteFloat(aWriter, Rotation.y);
    Serialization::WriteFloat(aWriter, Rotation.z);
    Serialization::WriteBool(aWriter, AtRest);
    Serialization::WriteVarInt(aWriter, Tick);
    Serialization::WriteVarInt(aWriter, MoverPlayerId);
    Serialization::WriteBool(aWriter, Thrown);
    if (Thrown)
    {
        Serialization::WriteFloat(aWriter, Velocity.x);
        Serialization::WriteFloat(aWriter, Velocity.y);
        Serialization::WriteFloat(aWriter, Velocity.z);
        Serialization::WriteFloat(aWriter, Spin.x);
        Serialization::WriteFloat(aWriter, Spin.y);
        Serialization::WriteFloat(aWriter, Spin.z);
    }
    Serialization::WriteBool(aWriter, Fixed);
}

void NotifyDroppedItemMove::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    Id = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Position.Deserialize(aReader);
    Rotation.x = Serialization::ReadFloat(aReader);
    Rotation.y = Serialization::ReadFloat(aReader);
    Rotation.z = Serialization::ReadFloat(aReader);
    AtRest = Serialization::ReadBool(aReader);
    Tick = Serialization::ReadVarInt(aReader);
    MoverPlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Thrown = Serialization::ReadBool(aReader);
    Velocity = {};
    Spin = {};
    if (Thrown)
    {
        Velocity.x = Serialization::ReadFloat(aReader);
        Velocity.y = Serialization::ReadFloat(aReader);
        Velocity.z = Serialization::ReadFloat(aReader);
        Spin.x = Serialization::ReadFloat(aReader);
        Spin.y = Serialization::ReadFloat(aReader);
        Spin.z = Serialization::ReadFloat(aReader);
    }
    Fixed = Serialization::ReadBool(aReader);
}
