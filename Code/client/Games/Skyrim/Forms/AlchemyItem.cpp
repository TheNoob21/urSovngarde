#include "AlchemyItem.h"

#include <Systems/ModSystem.h>
#include <Magic/EffectItem.h>
#include <World.h>
#include <Games/BGSCreatedObjectManager.h>

namespace
{
// The check that a function reads the made potions or the poisons the game keeps (at 0x38 and 0x68): one that does not is not called.
bool ReadsList(const void* apFunction, const uint8_t aOffset) noexcept
{
    // lea r64, [r64 + offset]: a REX.W prefix, 8D, a register with an 8-bit displacement, the offset.
    const auto* pCode = static_cast<const uint8_t*>(apFunction);
    for (size_t i = 0; pCode && i + 4 <= 0x60; ++i)
    {
        const uint8_t cRex = pCode[i];
        const uint8_t cModRm = pCode[i + 2];
        if ((cRex & 0xFA) == 0x48 && pCode[i + 1] == 0x8D && (cModRm & 0xC0) == 0x40 && (cModRm & 0x07) != 0x04 && pCode[i + 3] == aOffset)
            return true;
    }
    return false;
}
} // namespace

AlchemyItem* AlchemyItem::Create(const CreatedAlchemy& acData) noexcept
{
    // The game's own lookup: the made item with these effects, or a new one, in *apItem.
    TP_THIS_FUNCTION(TAddAlchemyItem, void, void, AlchemyItem**, GameArray<EffectItem>*);
    POINTER_SKYRIMSE(TAddAlchemyItem, addPotion, 36167, 35265);
    POINTER_SKYRIMSE(TAddAlchemyItem, addPoison, 36168, 35266);
    static const bool s_potionKnown = ReadsList(reinterpret_cast<const void*>(addPotion.Get()), 0x38);
    static const bool s_poisonKnown = ReadsList(reinterpret_cast<const void*>(addPoison.Get()), 0x68);

    if (!acData.IsSet() || !(acData.IsPoison ? s_poisonKnown : s_potionKnown))
    {
        spdlog::debug("Made {} not created: no effects, or this game's function to make one is not the one expected", acData.IsPoison ? "poison" : "potion");
        return nullptr;
    }

    ModSystem& modSystem = World::Get().GetModSystem();

    GameArray<EffectItem> effects{};
    effects.Resize(acData.Effects.size());
    for (size_t i = 0; i < acData.Effects.size(); ++i)
    {
        const ItemEffect& acEffect = acData.Effects[i];

        EffectItem effectItem{};
        effectItem.data.fMagnitude = acEffect.Magnitude;
        effectItem.data.iArea = acEffect.Area;
        effectItem.data.iDuration = acEffect.Duration;
        effectItem.fRawCost = acEffect.RawCost;
        effectItem.pEffectSetting = Cast<EffectSetting>(TESForm::GetById(modSystem.GetGameId(acEffect.EffectId)));
        // An effect this game lacks would make another item.
        if (!effectItem.pEffectSetting)
        {
            spdlog::debug("Made {}: effect {:X}:{:X} is not in this game; not created", acData.IsPoison ? "poison" : "potion", acEffect.EffectId.ModId, acEffect.EffectId.BaseId);
            return nullptr;
        }
        effects[static_cast<uint32_t>(i)] = effectItem;
    }

    AlchemyItem* pItem = nullptr;
    TiltedPhoques::ThisCall(acData.IsPoison ? addPoison.Get() : addPotion.Get(), static_cast<void*>(BGSCreatedObjectManager::Get()), &pItem, &effects);
    if (!pItem)
        return nullptr;

    // The game names and prices a made item from its effects, as the maker's game did, in this game's own words.
    // The call may hand back this player's own item with these effects: only what it lacks is filled in.
    if (!acData.Name.empty() && (!pItem->fullName.value.data || !*pItem->fullName.value.data))
        pItem->fullName.value.Set(acData.Name.c_str());
    if (acData.CostOverride >= 0 && !(pItem->data.uiFlags & kCostOverride))
    {
        pItem->data.iCostOverride = acData.CostOverride;
        pItem->data.uiFlags |= kCostOverride;
    }

    return pItem;
}
