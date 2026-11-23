#include <Messages/NotifyDroppedItem.h>

#include <DecodeGuard.h>

void NotifyDroppedItem::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Id);
    Item.Serialize(aWriter);
    CellId.Serialize(aWriter);
    WorldSpaceId.Serialize(aWriter);
    Position.Serialize(aWriter);
    Serialization::WriteFloat(aWriter, Rotation.x);
    Serialization::WriteFloat(aWriter, Rotation.y);
    Serialization::WriteFloat(aWriter, Rotation.z);
    Serialization::WriteVarInt(aWriter, RequestId);
    Serialization::WriteBool(aWriter, LegacyItemData);
    Serialization::WriteBool(aWriter, Replay);
    Serialization::WriteBool(aWriter, Fixed);
    const uint64_t cPlaces = std::min<uint64_t>(EarlierPlaces.size(), kMaxEarlierPlaces);
    Serialization::WriteVarInt(aWriter, cPlaces);
    for (uint64_t i = 0; i < cPlaces; ++i)
        EarlierPlaces[i].Serialize(aWriter);
}

void NotifyDroppedItem::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    Id = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Item.Deserialize(aReader);
    CellId.Deserialize(aReader);
    WorldSpaceId.Deserialize(aReader);
    Position.Deserialize(aReader);
    Rotation.x = Serialization::ReadFloat(aReader);
    Rotation.y = Serialization::ReadFloat(aReader);
    Rotation.z = Serialization::ReadFloat(aReader);
    RequestId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    LegacyItemData = Serialization::ReadBool(aReader);
    Replay = Serialization::ReadBool(aReader);
    Fixed = Serialization::ReadBool(aReader);
    EarlierPlaces.clear();
    // More than any server sends is a damaged message, dropped whole (DecodeGuard).
    const uint64_t cPlaces = Serialization::ReadVarInt(aReader);
    if (!DecodeGuard::Within(cPlaces, kMaxEarlierPlaces))
        return;
    for (uint64_t i = 0; i < cPlaces && !aReader.Eof(); ++i)
    {
        Vector3_NetQuantize place{};
        place.Deserialize(aReader);
        EarlierPlaces.push_back(place);
    }
}
