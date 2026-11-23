#include <Structs/Inventory.h>
#include <DecodeGuard.h>
#include <TiltedCore/Serialization.hpp>

using TiltedPhoques::Serialization;

void Inventory::Entry::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    BaseId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, Count);

    Serialization::WriteFloat(aWriter, ExtraCharge);

    ExtraEnchantId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, ExtraEnchantCharge);
    Serialization::WriteVarInt(aWriter, EnchantData.Effects.size());
    for (const EffectItem& effect : EnchantData.Effects)
    {
        effect.Serialize(aWriter);
    }

    Serialization::WriteFloat(aWriter, ExtraHealth);

    ExtraPoisonId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, ExtraPoisonCount);

    Serialization::WriteVarInt(aWriter, ExtraSoulLevel);

    Serialization::WriteBool(aWriter, EnchantData.IsWeapon);
    Serialization::WriteBool(aWriter, ExtraEnchantRemoveUnequip);
    Serialization::WriteBool(aWriter, ExtraWorn);
    Serialization::WriteBool(aWriter, ExtraWornLeft);
    Serialization::WriteBool(aWriter, IsQuestItem);

    Data.Serialize(aWriter);
}

void Inventory::Entry::Deserialize(TiltedPhoques::Buffer::Reader& aReader, const bool aWithData) noexcept
{
    BaseId.Deserialize(aReader);
    Count = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

    ExtraCharge = Serialization::ReadFloat(aReader);

    ExtraEnchantId.Deserialize(aReader);
    ExtraEnchantCharge = Serialization::ReadVarInt(aReader) & 0xFFFF;
    uint64_t effectCount = Serialization::ReadVarInt(aReader);
    for (uint64_t i = 0; i < effectCount && !aReader.Eof(); i++)
    {
        EffectItem effect;
        effect.Deserialize(aReader);
        // A message with more effects than any item has keeps only the first ones.
        if (EnchantData.Effects.size() < ItemEffects::kMaxEffects)
            EnchantData.Effects.push_back(effect);
    }

    ExtraHealth = Serialization::ReadFloat(aReader);

    ExtraPoisonId.Deserialize(aReader);
    ExtraPoisonCount = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

    ExtraSoulLevel = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

    EnchantData.IsWeapon = Serialization::ReadBool(aReader);
    ExtraEnchantRemoveUnequip = Serialization::ReadBool(aReader);
    ExtraWorn = Serialization::ReadBool(aReader);
    ExtraWornLeft = Serialization::ReadBool(aReader);
    IsQuestItem = Serialization::ReadBool(aReader);

    if (aWithData)
        Data.Deserialize(aReader);
}

bool Inventory::operator==(const Inventory& acRhs) const noexcept
{
    return Entries == acRhs.Entries;
}

bool Inventory::operator!=(const Inventory& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

bool Inventory::Entry::operator==(const Inventory::Entry& acRhs) const noexcept
{
    return BaseId == acRhs.BaseId && Count == acRhs.Count && IsExtraDataEquals(acRhs) && Data.Base == acRhs.Data.Base;
}

bool Inventory::Entry::operator!=(const Inventory::Entry& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void Inventory::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Entries.size());
    for (const Entry& entry : Entries)
    {
        entry.Serialize(aWriter);
    }

    CurrentMagicEquipment.Serialize(aWriter);
}

void Inventory::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    // No real container or player holds more stacks than this.
    constexpr uint64_t cMaxEntries = 65536;
    const uint64_t cCount = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    if (!DecodeGuard::Within(cCount, cMaxEntries))
        return;

    for (uint64_t i = 0; i < cCount && !aReader.Eof(); i++)
    {
        Entry entry;
        entry.Deserialize(aReader);
        Entries.push_back(entry);
    }

    CurrentMagicEquipment.Deserialize(aReader);
}

std::optional<Inventory::Entry> Inventory::GetEntryById(GameId& aItemId) const noexcept
{
    auto entry = std::find_if(Entries.begin(), Entries.end(), [aItemId](auto entry) { return entry.BaseId == aItemId; });
    if (entry == Entries.end())
        return std::nullopt;

    return {*entry};
}

int32_t Inventory::GetEntryCountById(GameId& aItemId) const noexcept
{
    auto entry = GetEntryById(aItemId);
    if (!entry)
        return 0;

    return entry->Count;
}

// TODO: unit testing
void Inventory::AddOrRemoveEntry(const Entry& acEntry) noexcept
{
    auto duplicate = std::find_if(Entries.begin(), Entries.end(), [acEntry](Entry& entry) { return entry.CanBeMerged(acEntry); });

    if (duplicate != Entries.end())
    {
        duplicate->Count += acEntry.Count;
        if (duplicate->Count <= 0)
            Entries.erase(duplicate);
    }
    else
    {
        Entries.push_back(acEntry);
    }
}

void Inventory::UpdateEquipment(const Inventory& acNewInventory) noexcept
{
    while (true)
    {
        auto wornEntry = std::find_if(Entries.begin(), Entries.end(), [](auto& aEntry) { return aEntry.IsWorn(); });
        if (wornEntry == Entries.end())
            break;

        wornEntry->ExtraWorn = wornEntry->ExtraWornLeft = false;
    }

    for (const auto& newEntry : acNewInventory.Entries)
    {
        if (!newEntry.IsWorn())
            continue;

        // The very stack first: an enchanted, tempered or named one is not the plain one of the same base.
        Entry stack = newEntry;
        stack.ExtraWorn = stack.ExtraWornLeft = false;
        auto entry = std::find_if(Entries.begin(), Entries.end(), [&stack](auto& aEntry) { return !aEntry.IsWorn() && aEntry.CanBeMerged(stack); });
        if (entry == Entries.end())
            entry = std::find_if(Entries.begin(), Entries.end(), [&newEntry](auto& aEntry) { return aEntry.BaseId == newEntry.BaseId && !aEntry.IsWorn(); });
        if (entry == Entries.end())
            entry = std::find_if(Entries.begin(), Entries.end(), [&newEntry](auto& aEntry) { return aEntry.BaseId == newEntry.BaseId; });

        // This shouldn't happen
        if (entry == Entries.end())
            continue;

        entry->ExtraWorn = newEntry.ExtraWorn;
        entry->ExtraWornLeft = newEntry.ExtraWornLeft;
    }

    CurrentMagicEquipment = acNewInventory.CurrentMagicEquipment;
}

void Inventory::RemoveByFilter(std::function<bool(const Entry&)> aFilter) noexcept
{
    Entries.erase(std::remove_if(Entries.begin(), Entries.end(), aFilter), Entries.end());
}

bool Inventory::ContainsQuestItems() const noexcept
{
    for (const auto& entry : Entries)
    {
        if (entry.IsQuestItem)
            return true;
    }
    return false;
}