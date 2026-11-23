#include <Messages/NotifyStuckArrow.h>

#include <DecodeGuard.h>
#include <StuckArrow.h>

void NotifyStuckArrow::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    Ammo.Serialize(aWriter);
    Serialization::WriteString(aWriter, Bone);
    for (int axis = 0; axis < 3; ++axis)
        Serialization::WriteFloat(aWriter, Position[axis]);
    for (int axis = 0; axis < 4; ++axis)
        Serialization::WriteFloat(aWriter, Rotation[axis]);
    Serialization::WriteFloat(aWriter, Scale);
    Serialization::WriteVarInt(aWriter, Sequence);
    Serialization::WriteBool(aWriter, Removed);
}

void NotifyStuckArrow::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Ammo.Deserialize(aReader);
    Bone = Serialization::ReadString(aReader);
    // A name no skeleton has is a damaged message: the factory drops it.
    if (!DecodeGuard::Within(Bone.size(), StuckArrow::kMaxBoneNameLength))
    {
        Bone.clear();
        return;
    }
    for (int axis = 0; axis < 3; ++axis)
        Position[axis] = Serialization::ReadFloat(aReader);
    for (int axis = 0; axis < 4; ++axis)
        Rotation[axis] = Serialization::ReadFloat(aReader);
    Scale = Serialization::ReadFloat(aReader);
    Sequence = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Removed = Serialization::ReadBool(aReader);
}
