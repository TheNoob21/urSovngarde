#include <Messages/NotifyItemDataChange.h>
#include <TiltedCore/Serialization.hpp>

void NotifyItemDataChange::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    Serialization::WriteVarInt(aWriter, OwnershipEpoch);
    Serialization::WriteVarInt(aWriter, Changes.size());
    for (const ItemDataChange& change : Changes)
        change.Serialize(aWriter);
}

void NotifyItemDataChange::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    OwnershipEpoch = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    const uint64_t cCount = Serialization::ReadVarInt(aReader);
    Changes.clear();
    for (uint64_t i = 0; i < cCount && !aReader.Eof(); ++i)
    {
        ItemDataChange change{};
        change.Deserialize(aReader);
        if (Changes.size() < ItemDataChanges::kMaxChanges)
            Changes.push_back(std::move(change));
    }
}
