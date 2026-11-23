#include <Structs/Factions.h>
#include <TiltedCore/Serialization.hpp>
#include <DecodeGuard.h>

using TiltedPhoques::Serialization;

bool Factions::operator==(const Factions& acRhs) const noexcept
{
    return NpcFactions == acRhs.NpcFactions && ExtraFactions == acRhs.ExtraFactions;
}

bool Factions::operator!=(const Factions& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void Factions::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    // Limit the number of factions to send
    Serialization::WriteVarInt(aWriter, NpcFactions.size() & 0x1FF);

    for (auto& entry : NpcFactions)
    {
        entry.Serialize(aWriter);
    }

    Serialization::WriteVarInt(aWriter, ExtraFactions.size() & 0x1FF);

    for (auto& entry : ExtraFactions)
    {
        entry.Serialize(aWriter);
    }
}

void Factions::Deserialize(TiltedPhoques::Buffer::Reader& aReader)
{
    // A count past what a message holds is a damaged message: fail it and read nothing more. The readers are noexcept, so this must not throw.
    constexpr uint64_t cMaxFactions = 0x1FF;

    const auto npcCount = Serialization::ReadVarInt(aReader);
    if (!DecodeGuard::Within(npcCount, cMaxFactions))
        return;

    NpcFactions.resize(npcCount);
    for (auto& entry : NpcFactions)
    {
        entry.Deserialize(aReader);
    }

    const auto extraCount = Serialization::ReadVarInt(aReader);
    if (!DecodeGuard::Within(extraCount, cMaxFactions))
        return;

    ExtraFactions.resize(extraCount);
    for (auto& entry : ExtraFactions)
    {
        entry.Deserialize(aReader);
    }
}
