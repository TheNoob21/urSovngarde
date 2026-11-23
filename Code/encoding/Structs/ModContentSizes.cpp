#include <Structs/ModContentSizes.h>

#include <Structs/PluginList.h>

using TiltedPhoques::Serialization;

void ModContentSizes::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    const auto cCount = Sizes.size() <= PluginList::kMax ? Sizes.size() : 0;
    aWriter.WriteBits(static_cast<uint8_t>(GameEdition), 2);
    Serialization::WriteVarInt(aWriter, cCount);
    for (size_t i = 0; i < cCount; ++i)
        Serialization::WriteVarInt(aWriter, Sizes[i]);
}

void ModContentSizes::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    *this = {};
    uint64_t edition = 0;
    if (!aReader.ReadBits(edition, 2))
        return;
    if (!DecodeGuard::Within(edition, static_cast<uint8_t>(Edition::kVR)))
        return;

    GameEdition = static_cast<Edition>(edition);
    const auto cCount = Serialization::ReadVarInt(aReader);
    if (!DecodeGuard::Within(cCount, PluginList::kMax))
        return;
    Sizes.resize(cCount);
    for (auto& size : Sizes)
        size = Serialization::ReadVarInt(aReader);
}
