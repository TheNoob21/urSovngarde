#include <Messages/NotifyItemTakeLost.h>

void NotifyItemTakeLost::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, static_cast<uint8_t>(From));
    Serialization::WriteVarInt(aWriter, Id);
    ObjectId.Serialize(aWriter);
    Item.Serialize(aWriter);
}

void NotifyItemTakeLost::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    From = static_cast<ItemTakeSource>(Serialization::ReadVarInt(aReader) & 0xFF);
    Id = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    ObjectId.Deserialize(aReader);
    Item.Deserialize(aReader);
}
