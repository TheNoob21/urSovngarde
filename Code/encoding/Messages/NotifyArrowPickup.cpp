#include <Messages/NotifyArrowPickup.h>

void NotifyArrowPickup::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    ProjectileBaseId.Serialize(aWriter);
    CellId.Serialize(aWriter);
    Serialization::WriteFloat(aWriter, X);
    Serialization::WriteFloat(aWriter, Y);
    Serialization::WriteFloat(aWriter, Z);
}

void NotifyArrowPickup::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    ProjectileBaseId.Deserialize(aReader);
    CellId.Deserialize(aReader);
    X = Serialization::ReadFloat(aReader);
    Y = Serialization::ReadFloat(aReader);
    Z = Serialization::ReadFloat(aReader);
}
