#include <Structs/Mods.h>
#include <BoundedString.h>
#include <DecodeGuard.h>
#include <TiltedCore/Serialization.hpp>
#include <algorithm>

using TiltedPhoques::Serialization;

bool Mods::operator==(const Mods& acRhs) const noexcept
{
    return ModList == acRhs.ModList;
}

bool Mods::operator!=(const Mods& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void Mods::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    const uint16_t modCount = std::min(ModList.size(), size_t(4096)) & 0xFFFF;
    aWriter.WriteBits(modCount, 13);

    for (auto& entry : ModList)
    {
        aWriter.WriteBits(entry.Id, 16);
        Serialization::WriteBool(aWriter, entry.IsLite);
        Serialization::WriteString(aWriter, entry.Filename);
    }
}

void Mods::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t data = 0;
    aReader.ReadBits(data, 13);

    // The most a list holds (Serialize cuts it there), and a plugin file name is short: a login with more is not a real game.
    constexpr uint64_t cMaxMods = 4096;
    constexpr uint64_t cMaxFilenameLength = 256;
    const size_t modCount = data & 0xFFFF;
    if (!DecodeGuard::Within(modCount, cMaxMods))
        return;
    ModList.resize(modCount);
    for (size_t i = 0; i < modCount; ++i)
    {
        aReader.ReadBits(data, 16);
        ModList[i].Id = data & 0xFFFF;
        ModList[i].IsLite = Serialization::ReadBool(aReader);
        ModList[i].Filename = BoundedString::Read(aReader, cMaxFilenameLength);
        if (DecodeGuard::t_failed)
        {
            ModList.clear();
            return;
        }
    }
}
