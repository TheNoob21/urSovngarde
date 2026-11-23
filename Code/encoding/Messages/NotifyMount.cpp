#include <Messages/NotifyMount.h>

void NotifyMount::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, RiderId);
    Serialization::WriteVarInt(aWriter, MountId);
    Serialization::WriteVarInt(aWriter, RiderOwnershipEpoch);
    Serialization::WriteVarInt(aWriter, MountOwnershipEpoch);
    Serialization::WriteBool(aWriter, IsRefresh);
}

void NotifyMount::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    RiderId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    MountId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    RiderOwnershipEpoch = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    MountOwnershipEpoch = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    IsRefresh = Serialization::ReadBool(aReader);
}
