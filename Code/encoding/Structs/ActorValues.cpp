#include <Structs/ActorValues.h>
#include <DecodeGuard.h>
#include <Structs/ActorValueLimits.h>
#include <TiltedCore/Serialization.hpp>

using TiltedPhoques::Serialization;

bool ActorValues::operator==(const ActorValues& acRhs) const noexcept
{
    return ActorValuesList == acRhs.ActorValuesList;
}

bool ActorValues::operator!=(const ActorValues& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void ActorValues::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ActorValuesList.size());
    for (auto& value : ActorValuesList)
    {
        Serialization::WriteVarInt(aWriter, value.first);
        Serialization::WriteFloat(aWriter, value.second);
    }

    Serialization::WriteVarInt(aWriter, ActorMaxValuesList.size());
    for (auto& value : ActorMaxValuesList)
    {
        Serialization::WriteVarInt(aWriter, value.first);
        Serialization::WriteFloat(aWriter, value.second);
    }
}

void ActorValues::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    const auto count = Serialization::ReadVarInt(aReader);
    if (!DecodeGuard::Within(count, ActorValueLimits::kMaxEntries))
        return;
    for (uint64_t i = 0; i < count; i++)
    {
        const auto key = Serialization::ReadVarInt(aReader);
        const auto value = Serialization::ReadFloat(aReader);
        if (ActorValueLimits::IsKnownKey(key))
            ActorValuesList.insert({static_cast<uint32_t>(key), value});
    }

    const auto maxCount = Serialization::ReadVarInt(aReader);
    if (!DecodeGuard::Within(maxCount, ActorValueLimits::kMaxEntries))
        return;
    for (uint64_t i = 0; i < maxCount; i++)
    {
        const auto key = Serialization::ReadVarInt(aReader);
        const auto value = Serialization::ReadFloat(aReader);
        if (ActorValueLimits::IsKnownKey(key))
            ActorMaxValuesList.insert({static_cast<uint32_t>(key), value});
    }
}
