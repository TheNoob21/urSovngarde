#pragma once

#include <Structs/GameId.h>
#include <Structs/Inventory.h>
#include <Structs/Vector3_NetQuantize.h>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <vector>

struct DroppedItemRecord
{
    //! The file's version. Placed objects' resting places follow the items in a tagged trailer, so a server that knows
    //! only version 4 still reads the items and stops before them.
    static constexpr uint64_t kVersion = 4;

    bool EnrichItemData(const ItemData& acData)
    {
        // A replay-created plain copy cannot prove that the original had no metadata.
        if (!LegacyItemData || (!acData.HasExtra() && !acData.Base.IsSet()))
            return false;
        Entry.Data = acData;
        LegacyItemData = false;
        return true;
    }

    //! At most this many earlier resting places are kept: the first (where it was dropped) and the latest others.
    static constexpr size_t kMaxEarlierPlaces = 4;

    //! The item came to rest at acAt. Once that is farther than aRadius from where it last rested, the old resting place is
    //! kept as an earlier place: a save made while it lay there still has it there, and a game reconnecting with that save
    //! finds its copy by it instead of placing a second one. A smaller move keeps the old resting place as the anchor, so
    //! nudges add up. The first place is never dropped; past kMaxEarlierPlaces the oldest of the others is.
    void NoteRest(const glm::vec3& acAt, const float aRadius)
    {
        if (!(glm::distance(acAt, glm::vec3(RestPosition)) > aRadius))
            return;
        const bool cKnown = std::any_of(
            EarlierPlaces.begin(), EarlierPlaces.end(), [&](const Vector3_NetQuantize& acPlace) { return glm::distance(glm::vec3(acPlace), glm::vec3(RestPosition)) <= aRadius; });
        if (!cKnown)
        {
            EarlierPlaces.push_back(RestPosition);
            if (EarlierPlaces.size() > kMaxEarlierPlaces)
                EarlierPlaces.erase(EarlierPlaces.begin() + 1);
        }
        RestPosition = acAt;
    }

    //! The distance from acAt to where the item lies, or to the nearest of its earlier resting places when that is nearer.
    float NearestPlaceDistance(const glm::vec3& acAt) const noexcept
    {
        float nearest = glm::distance(glm::vec3(Position), acAt);
        for (const Vector3_NetQuantize& cPlace : EarlierPlaces)
            nearest = std::min(nearest, glm::distance(glm::vec3(cPlace), acAt));
        return nearest;
    }

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
    {
        using TiltedPhoques::Serialization;
        Serialization::WriteVarInt(aWriter, Id);
        Entry.Serialize(aWriter);
        CellId.Serialize(aWriter);
        WorldSpaceId.Serialize(aWriter);
        Position.Serialize(aWriter);
        Serialization::WriteFloat(aWriter, Rotation.x);
        Serialization::WriteFloat(aWriter, Rotation.y);
        Serialization::WriteFloat(aWriter, Rotation.z);
        Serialization::WriteBool(aWriter, LegacyItemData);
    }

    void Deserialize(TiltedPhoques::Buffer::Reader& aReader, uint64_t aVersion) noexcept
    {
        using TiltedPhoques::Serialization;
        Id = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
        Entry.Deserialize(aReader, aVersion >= 3);
        CellId.Deserialize(aReader);
        WorldSpaceId.Deserialize(aReader);
        Position.Deserialize(aReader);
        if (aVersion >= 2)
        {
            Rotation.x = Serialization::ReadFloat(aReader);
            Rotation.y = Serialization::ReadFloat(aReader);
            Rotation.z = Serialization::ReadFloat(aReader);
        }
        LegacyItemData = aVersion < 3 || (aVersion >= 4 && Serialization::ReadBool(aReader));
        RestPosition = Position;
    }

    uint32_t Id{};
    Inventory::Entry Entry{};
    GameId CellId{};
    GameId WorldSpaceId{};
    Vector3_NetQuantize Position{};
    glm::vec3 Rotation{};
    bool LegacyItemData{};
    //! Held still where it lies by a script (a weapon rack's weapon). Kept in the file's own trailer (DroppedItemFile::kFixedTag).
    bool Fixed{};
    //! Where it rested before it was moved away, first the place it was dropped (NoteRest). Kept in the file's own trailer
    //! (DroppedItemFile::kPlacesTag) and sent with the item (NotifyDroppedItem::EarlierPlaces).
    std::vector<Vector3_NetQuantize> EarlierPlaces;
    //! Where it last came to rest, the anchor NoteRest measures from. Not kept: a loaded item rests where it lies.
    Vector3_NetQuantize RestPosition{};
};

//! Where a moved placed world object came to rest, kept by the server across restarts.
struct PlacedObjectRecord
{
    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
    {
        using TiltedPhoques::Serialization;
        ObjectId.Serialize(aWriter);
        CellId.Serialize(aWriter);
        WorldSpaceId.Serialize(aWriter);
        Position.Serialize(aWriter);
        Serialization::WriteFloat(aWriter, Rotation.x);
        Serialization::WriteFloat(aWriter, Rotation.y);
        Serialization::WriteFloat(aWriter, Rotation.z);
    }

    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
    {
        using TiltedPhoques::Serialization;
        ObjectId.Deserialize(aReader);
        CellId.Deserialize(aReader);
        WorldSpaceId.Deserialize(aReader);
        Position.Deserialize(aReader);
        Rotation.x = Serialization::ReadFloat(aReader);
        Rotation.y = Serialization::ReadFloat(aReader);
        Rotation.z = Serialization::ReadFloat(aReader);
    }

    GameId ObjectId{};
    GameId CellId{};
    GameId WorldSpaceId{};
    Vector3_NetQuantize Position{};
    glm::vec3 Rotation{};
};

// The server's file of dropped items and placed objects' resting places: its layout, written and read whole.
namespace DroppedItemFile
{
inline constexpr uint32_t kMagic = 0x504F5244; // "DROP"
//! Opens and closes the placed objects' trailer; a trailer cut short has no closing tag and is not read.
inline constexpr uint32_t kObjectsTag = 0x534A424F; // "OBJS"
inline constexpr uint64_t kObjectsVersion = 1;
//! Opens and closes the trailer of the items held still by a script, after the objects' trailer; older servers stop before it.
inline constexpr uint32_t kFixedTag = 0x44584946; // "FIXD"
inline constexpr uint64_t kFixedVersion = 1;
//! Opens and closes the trailer of the items' earlier resting places, after the fixed items' trailer; older servers stop before it,
//! and a file without it (written before it existed) loads with no earlier places.
inline constexpr uint32_t kPlacesTag = 0x53434C50; // "PLCS"
inline constexpr uint64_t kPlacesVersion = 1;
//! Files of version 5 held the objects straight after the items, untagged; they are still read.
inline constexpr uint64_t kInlineObjectsVersion = 5;
//! At most this many resting places are kept; far more than a session moves.
inline constexpr uint64_t kMaxObjects = 4096;

struct Contents
{
    bool Valid{};
    uint64_t Version{};
    uint32_t NextId{1};
    std::vector<DroppedItemRecord> Items;
    std::vector<PlacedObjectRecord> Objects;
};

template <class TItems, class TObjects> void Write(TiltedPhoques::Buffer::Writer& aWriter, uint32_t aNextId, const TItems& acItems, const TObjects& acObjects) noexcept
{
    using TiltedPhoques::Serialization;
    aWriter.WriteBits(kMagic, 32);
    Serialization::WriteVarInt(aWriter, DroppedItemRecord::kVersion);
    Serialization::WriteVarInt(aWriter, aNextId);
    Serialization::WriteVarInt(aWriter, acItems.size());
    for (const DroppedItemRecord& cItem : acItems)
        cItem.Serialize(aWriter);

    const uint64_t cObjects = std::min<uint64_t>(acObjects.size(), kMaxObjects);
    aWriter.WriteBits(kObjectsTag, 32);
    Serialization::WriteVarInt(aWriter, kObjectsVersion);
    Serialization::WriteVarInt(aWriter, cObjects);
    uint64_t written = 0;
    for (const PlacedObjectRecord& cObject : acObjects)
    {
        if (written++ == cObjects)
            break;
        cObject.Serialize(aWriter);
    }
    aWriter.WriteBits(kObjectsTag, 32);

    uint64_t cFixed = 0;
    for (const DroppedItemRecord& cItem : acItems)
        cFixed += cItem.Fixed ? 1 : 0;
    aWriter.WriteBits(kFixedTag, 32);
    Serialization::WriteVarInt(aWriter, kFixedVersion);
    Serialization::WriteVarInt(aWriter, cFixed);
    for (const DroppedItemRecord& cItem : acItems)
        if (cItem.Fixed)
            Serialization::WriteVarInt(aWriter, cItem.Id);
    aWriter.WriteBits(kFixedTag, 32);

    uint64_t cPlaced = 0;
    for (const DroppedItemRecord& cItem : acItems)
        cPlaced += cItem.EarlierPlaces.empty() ? 0 : 1;
    aWriter.WriteBits(kPlacesTag, 32);
    Serialization::WriteVarInt(aWriter, kPlacesVersion);
    Serialization::WriteVarInt(aWriter, cPlaced);
    for (const DroppedItemRecord& cItem : acItems)
    {
        if (cItem.EarlierPlaces.empty())
            continue;
        const size_t cCount = std::min(cItem.EarlierPlaces.size(), DroppedItemRecord::kMaxEarlierPlaces);
        Serialization::WriteVarInt(aWriter, cItem.Id);
        Serialization::WriteVarInt(aWriter, cCount);
        for (size_t i = 0; i < cCount; ++i)
            cItem.EarlierPlaces[i].Serialize(aWriter);
    }
    aWriter.WriteBits(kPlacesTag, 32);
}

// The items' earlier resting places, applied to aItems only when the whole trailer is there and sound.
inline void ReadPlaces(TiltedPhoques::Buffer::Reader& aReader, std::vector<DroppedItemRecord>& aItems) noexcept
{
    using TiltedPhoques::Serialization;
    uint64_t tag = 0;
    if (!aReader.ReadBits(tag, 32) || tag != kPlacesTag || Serialization::ReadVarInt(aReader) != kPlacesVersion)
        return;
    const uint64_t cCount = Serialization::ReadVarInt(aReader);
    if (cCount > aItems.size())
        return;
    std::vector<std::pair<uint32_t, std::vector<Vector3_NetQuantize>>> places;
    for (uint64_t i = 0; i < cCount && !aReader.Eof(); ++i)
    {
        const auto cId = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
        const uint64_t cPlaces = Serialization::ReadVarInt(aReader);
        if (cPlaces == 0 || cPlaces > DroppedItemRecord::kMaxEarlierPlaces)
            return;
        std::vector<Vector3_NetQuantize> list(static_cast<size_t>(cPlaces));
        for (auto& place : list)
            place.Deserialize(aReader);
        places.emplace_back(cId, std::move(list));
    }
    if (places.size() != cCount || !aReader.ReadBits(tag, 32) || tag != kPlacesTag)
        return;
    for (auto& [id, list] : places)
        for (DroppedItemRecord& item : aItems)
            if (item.Id == id)
                item.EarlierPlaces = list;
}

// The ids of the items held still, applied to aItems only when the whole trailer is there.
inline void ReadFixed(TiltedPhoques::Buffer::Reader& aReader, std::vector<DroppedItemRecord>& aItems) noexcept
{
    using TiltedPhoques::Serialization;
    uint64_t tag = 0;
    if (!aReader.ReadBits(tag, 32) || tag != kFixedTag || Serialization::ReadVarInt(aReader) != kFixedVersion)
        return;
    const uint64_t cCount = Serialization::ReadVarInt(aReader);
    if (cCount > aItems.size())
        return;
    std::vector<uint32_t> ids;
    for (uint64_t i = 0; i < cCount && !aReader.Eof(); ++i)
        ids.push_back(static_cast<uint32_t>(Serialization::ReadVarInt(aReader)));
    if (ids.size() != cCount || !aReader.ReadBits(tag, 32) || tag != kFixedTag)
        return;
    for (DroppedItemRecord& item : aItems)
        item.Fixed = std::find(ids.begin(), ids.end(), item.Id) != ids.end();
}

inline void ReadObjects(TiltedPhoques::Buffer::Reader& aReader, uint64_t aCount, std::vector<PlacedObjectRecord>& aObjects) noexcept
{
    for (uint64_t i = 0; i < std::min(aCount, kMaxObjects) && !aReader.Eof(); ++i)
    {
        PlacedObjectRecord object{};
        object.Deserialize(aReader);
        aObjects.push_back(object);
    }
}

// Not valid, and nothing read, for a file of another kind or a version this server does not know.
// The items of a valid file are kept even when the objects' trailer after them is missing, unknown or cut short.
inline Contents Read(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    using TiltedPhoques::Serialization;
    Contents contents{};
    uint64_t magic = 0;
    aReader.ReadBits(magic, 32);
    contents.Version = Serialization::ReadVarInt(aReader);
    if (magic != kMagic || contents.Version < 1 || contents.Version > kInlineObjectsVersion)
        return contents;
    contents.Valid = true;

    contents.NextId = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
    const uint64_t cItems = Serialization::ReadVarInt(aReader);
    for (uint64_t i = 0; i < cItems && !aReader.Eof(); ++i)
    {
        DroppedItemRecord item{};
        item.Deserialize(aReader, contents.Version);
        contents.Items.push_back(item);
    }
    if (aReader.Eof())
        return contents;

    if (contents.Version == kInlineObjectsVersion)
    {
        ReadObjects(aReader, Serialization::ReadVarInt(aReader), contents.Objects);
        return contents;
    }

    // Reads past the end fail and give zeros, so a trailer cut short ends without its closing tag.
    uint64_t tag = 0;
    if (!aReader.ReadBits(tag, 32) || tag != kObjectsTag || Serialization::ReadVarInt(aReader) != kObjectsVersion)
        return contents;
    const uint64_t cObjects = Serialization::ReadVarInt(aReader);
    if (cObjects > kMaxObjects)
        return contents;
    std::vector<PlacedObjectRecord> objects;
    ReadObjects(aReader, cObjects, objects);
    if (objects.size() != cObjects || !aReader.ReadBits(tag, 32) || tag != kObjectsTag)
        return contents;
    contents.Objects = std::move(objects);
    ReadFixed(aReader, contents.Items);
    ReadPlaces(aReader, contents.Items);
    return contents;
}
} // namespace DroppedItemFile
