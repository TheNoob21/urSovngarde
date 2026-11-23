#include <Messages/RequestActorMaxValueChanges.h>
#include <DecodeGuard.h>
#include <Structs/ActorValueLimits.h>

void RequestActorMaxValueChanges::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Id);
    Serialization::WriteVarInt(aWriter, OwnershipEpoch);

    Serialization::WriteVarInt(aWriter, Values.size());
    for (const auto& value : Values)
    {
        Serialization::WriteVarInt(aWriter, value.first);
        Serialization::WriteFloat(aWriter, value.second);
    }
}

void RequestActorMaxValueChanges::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Id = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    OwnershipEpoch = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

    const auto count = Serialization::ReadVarInt(aReader);
    if (!DecodeGuard::Within(count, ActorValueLimits::kMaxEntries))
        return;
    for (uint64_t i = 0; i < count; i++)
    {
        const auto key = Serialization::ReadVarInt(aReader);
        const auto value = Serialization::ReadFloat(aReader);
        if (ActorValueLimits::IsKnownKey(key))
            Values.insert({static_cast<uint32_t>(key), value});
    }
}
