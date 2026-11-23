#include <Messages/RequestArrowPickup.h>

void RequestArrowPickup::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    ProjectileBaseId.Serialize(aWriter);
    CellId.Serialize(aWriter);
    Serialization::WriteFloat(aWriter, X);
    Serialization::WriteFloat(aWriter, Y);
    Serialization::WriteFloat(aWriter, Z);
}

void RequestArrowPickup::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    ProjectileBaseId.Deserialize(aReader);
    CellId.Deserialize(aReader);
    X = Serialization::ReadFloat(aReader);
    Y = Serialization::ReadFloat(aReader);
    Z = Serialization::ReadFloat(aReader);
}
