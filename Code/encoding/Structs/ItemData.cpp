#include <Structs/ItemData.h>
#include <DecodeGuard.h>
#include <TiltedCore/Serialization.hpp>
#include <BoundedString.h>

using TiltedPhoques::Serialization;

void ItemEffect::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteFloat(aWriter, Magnitude);
    Serialization::WriteVarInt(aWriter, Area);
    Serialization::WriteVarInt(aWriter, Duration);
    Serialization::WriteFloat(aWriter, RawCost);
    EffectId.Serialize(aWriter);
}

void ItemEffect::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Magnitude = Serialization::ReadFloat(aReader);
    Area = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Duration = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    RawCost = Serialization::ReadFloat(aReader);
    EffectId.Deserialize(aReader);
    // A number that is not a number, or beyond what a game holds, would reach a form kept in the receiver's save.
    ItemEffects::KeepSane(*this);
}

void CreatedAlchemy::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteString(aWriter, Name);
    Serialization::WriteVarInt(aWriter, static_cast<uint32_t>(CostOverride));
    Serialization::WriteBool(aWriter, IsPoison);
    Serialization::WriteVarInt(aWriter, Effects.size());
    for (const ItemEffect& effect : Effects)
        effect.Serialize(aWriter);
}

void CreatedAlchemy::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Name = BoundedString::Read(aReader, BoundedString::kLargest);
    if (Name.size() > kMaxNameLength)
        Name.resize(kMaxNameLength);
    CostOverride = static_cast<int32_t>(Serialization::ReadVarInt(aReader) & 0xFFFFFFFF);
    IsPoison = Serialization::ReadBool(aReader);
    const uint64_t cCount = Serialization::ReadVarInt(aReader);
    Effects.clear();
    for (uint64_t i = 0; i < cCount && !aReader.Eof(); ++i)
    {
        ItemEffect effect{};
        effect.Deserialize(aReader);
        if (Effects.size() < ItemEffects::kMaxEffects)
            Effects.push_back(effect);
    }
}

void ItemData::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteBool(aWriter, !DisplayName.empty());
    if (!DisplayName.empty())
        Serialization::WriteString(aWriter, DisplayName);
    Serialization::WriteBool(aWriter, Base.IsSet());
    if (Base.IsSet())
        Base.Serialize(aWriter);
    Serialization::WriteBool(aWriter, Poison.IsSet());
    if (Poison.IsSet())
        Poison.Serialize(aWriter);
}

void ItemData::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    *this = {};
    if (Serialization::ReadBool(aReader))
    {
        DisplayName = BoundedString::Read(aReader, BoundedString::kLargest);
        if (DisplayName.size() > CreatedAlchemy::kMaxNameLength)
            DisplayName.resize(CreatedAlchemy::kMaxNameLength);
    }
    if (Serialization::ReadBool(aReader))
        Base.Deserialize(aReader);
    if (Serialization::ReadBool(aReader))
        Poison.Deserialize(aReader);
}
