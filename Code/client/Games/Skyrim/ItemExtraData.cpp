#include <TiltedOnlinePCH.h>

#include <ItemExtraData.h>

#include <World.h>
#include <Games/Overrides.h>
#include <Games/Memory.h>
#include <TESObjectREFR.h>
#include <ExtraData/ExtraDataList.h>
#include <ExtraData/ExtraCharge.h>
#include <ExtraData/ExtraContainerChanges.h>
#include <ExtraData/ExtraCount.h>
#include <ExtraData/ExtraEnchantment.h>
#include <ExtraData/ExtraHealth.h>
#include <ExtraData/ExtraPoison.h>
#include <ExtraData/ExtraSoul.h>
#include <ExtraData/ExtraTextDisplayData.h>
#include <Forms/AlchemyItem.h>
#include <Forms/EnchantmentItem.h>

namespace
{
constexpr uint32_t kRunTimeMod = ItemDataChanges::kRunTimeMod;
// ExtraTextDisplayData::iOwnerInstance of a name a player gave.
constexpr int32_t kCustomName = -2;

bool IsRunTime(const uint32_t aFormId) noexcept
{
    return (aFormId & 0xFF000000) == 0xFF000000;
}

bool Describe(const MagicItem& acItem, TiltedPhoques::Vector<ItemEffect>& aEffects) noexcept
{
    auto& modSystem = World::Get().GetModSystem();
    aEffects.clear();
    for (const EffectItem* pEffectItem : acItem.listOfEffects)
    {
        if (!pEffectItem || !pEffectItem->pEffectSetting)
            return false;
        ItemEffect effect{};
        effect.Magnitude = pEffectItem->data.fMagnitude;
        effect.Area = pEffectItem->data.iArea;
        effect.Duration = pEffectItem->data.iDuration;
        effect.RawCost = pEffectItem->fRawCost;
        // An effect of a plugin the others lack: they would make another item.
        if (!modSystem.GetServerModId(pEffectItem->pEffectSetting->formID, effect.EffectId) || effect.EffectId.ModId == kRunTimeMod)
            return false;
        aEffects.push_back(effect);
    }
    return !aEffects.empty() && aEffects.size() <= ItemEffects::kMaxEffects;
}

bool Describe(const AlchemyItem& acItem, CreatedAlchemy& aData) noexcept
{
    aData = {};
    if (!Describe(static_cast<const MagicItem&>(acItem), aData.Effects))
    {
        aData = {};
        return false;
    }
    aData.IsPoison = acItem.IsPoison();
    aData.CostOverride = (acItem.data.uiFlags & AlchemyItem::kCostOverride) ? acItem.data.iCostOverride : -1;
    if (const char* pName = acItem.fullName.value.data)
        aData.Name = pName;
    return true;
}

// The made items found or made here, by the id they are sent by. A form the game has since let go is made again.
AlchemyItem* Make(const CreatedAlchemy& acData) noexcept
{
    static TiltedPhoques::Map<uint32_t, uint32_t> s_made;

    const uint32_t cKey = acData.Key();
    if (const auto it = s_made.find(cKey); it != s_made.end())
    {
        AlchemyItem* pKnown = Cast<AlchemyItem>(TESForm::GetById(it->second));
        CreatedAlchemy known{};
        if (pKnown && pKnown->IsCreated() && Describe(*pKnown, known) && known.Key() == cKey)
            return pKnown;
        s_made.erase(it);
    }

    AlchemyItem* pItem = AlchemyItem::Create(acData);
    if (!pItem)
        return nullptr;
    // A game whose function makes the other kind is not trusted with it again.
    if (pItem->IsPoison() != acData.IsPoison)
    {
        spdlog::debug("Made {} {:X} came out as a {}; not used", acData.IsPoison ? "poison" : "potion", pItem->formID, pItem->IsPoison() ? "poison" : "potion");
        return nullptr;
    }
    s_made[cKey] = pItem->formID;
    return pItem;
}

EnchantmentItem* GetEnchantment(const Inventory::Entry& acEntry) noexcept
{
    if (!acEntry.ExtraEnchantId)
        return nullptr;
    if (acEntry.ExtraEnchantId.ModId == kRunTimeMod)
        return acEntry.EnchantData.Effects.empty() ? nullptr : EnchantmentItem::Create(acEntry.EnchantData);
    return Cast<EnchantmentItem>(TESForm::GetById(World::Get().GetModSystem().GetGameId(acEntry.ExtraEnchantId)));
}

// The items of a base that have no extra data of their own.
int64_t PlainCount(TESObjectREFR* apOwner, TESBoundObject* apObject, const ExtraContainerChanges::Entry* apEntry) noexcept
{
    int64_t count = apOwner->GetItemCountInInventory(apObject);
    if (apEntry && apEntry->dataList)
    {
        for (ExtraDataList* pList : *apEntry->dataList)
        {
            if (!pList)
                continue;
            const auto* pCount = Cast<ExtraCount>(pList->GetByType(ExtraDataType::Count));
            count -= pCount ? pCount->count : 1;
        }
    }
    return count;
}

template <class T> T* Find(ExtraDataList* apExtraDataList) noexcept
{
    return Cast<T>(apExtraDataList->GetByType(T::eExtraData));
}

void Drop(ExtraDataList* apExtraDataList, const ExtraDataType aType) noexcept
{
    if (BSExtraData* pData = apExtraDataList->GetByType(aType))
        apExtraDataList->Remove(aType, pData);
}

ExtraTextDisplayData* FindCustomName(ExtraDataList* apExtraDataList) noexcept
{
    auto* pText = Find<ExtraTextDisplayData>(apExtraDataList);
    return pText && pText->iOwnerInstance == kCustomName && !pText->pDisplayNameText ? pText : nullptr;
}
} // namespace

namespace ItemExtraData
{
bool SetBase(Inventory::Entry& arEntry, const TESForm* apBase) noexcept
{
    arEntry.BaseId = {};
    arEntry.Data.Base = {};
    if (!apBase || !World::Get().GetModSystem().GetServerModId(apBase->formID, arEntry.BaseId))
        return false;
    if (!IsRunTime(apBase->formID))
        return true;

    const auto* pAlchemy = Cast<AlchemyItem>(const_cast<TESForm*>(apBase));
    if (!pAlchemy || !Describe(*pAlchemy, arEntry.Data.Base))
    {
        spdlog::debug("Item {:X} is a run-time form no other game can make; not synced", apBase->formID);
        arEntry.BaseId = {};
        arEntry.Data.Base = {};
        return false;
    }
    arEntry.BaseId = GameId(kRunTimeMod, arEntry.Data.Base.Key());
    return true;
}

void Read(Inventory::Entry& arEntry, ExtraDataList* apExtraDataList) noexcept
{
    arEntry.Data.DisplayName.clear();
    arEntry.Data.Poison = {};
    if (!apExtraDataList)
        return;

    if (const auto* pText = FindCustomName(apExtraDataList); pText && pText->DisplayName.data && *pText->DisplayName.data)
    {
        // The game keeps the name with its temper word after it ("Blade (Fine)"); the player's part is the custom length.
        arEntry.Data.DisplayName = pText->DisplayName.data;
        if (pText->usCustomNameLength > 0 && pText->usCustomNameLength < arEntry.Data.DisplayName.size())
            arEntry.Data.DisplayName.resize(pText->usCustomNameLength);
        if (arEntry.Data.DisplayName.size() > CreatedAlchemy::kMaxNameLength)
            arEntry.Data.DisplayName.resize(CreatedAlchemy::kMaxNameLength);
    }

    if (arEntry.ExtraPoisonId.ModId == kRunTimeMod)
    {
        const auto* pPoison = Find<ExtraPoison>(apExtraDataList);
        if (pPoison && pPoison->pPoison && Describe(*pPoison->pPoison, arEntry.Data.Poison))
            arEntry.ExtraPoisonId = GameId(kRunTimeMod, arEntry.Data.Poison.Key());
        else
        {
            // A poison no other game can make: the weapon goes without it.
            arEntry.ExtraPoisonId = {};
            arEntry.ExtraPoisonCount = 0;
            arEntry.Data.Poison = {};
        }
    }

    // A made enchantment is made again from its effects; the id says which one, the same in every game.
    if (arEntry.ExtraEnchantId.ModId == kRunTimeMod && !arEntry.EnchantData.Effects.empty())
        arEntry.ExtraEnchantId = GameId(kRunTimeMod, ItemEffects::Key(arEntry.EnchantData.Effects, arEntry.EnchantData.IsWeapon ? 3 : 4));
}

TESBoundObject* GetBase(const Inventory::Entry& acEntry) noexcept
{
    if (acEntry.BaseId.ModId == kRunTimeMod)
        return acEntry.Data.Base.IsSet() ? Make(acEntry.Data.Base) : nullptr;
    return Cast<TESBoundObject>(TESForm::GetById(World::Get().GetModSystem().GetGameId(acEntry.BaseId)));
}

AlchemyItem* GetPoison(const Inventory::Entry& acEntry) noexcept
{
    if (!acEntry.ExtraPoisonId)
        return nullptr;
    if (acEntry.ExtraPoisonId.ModId == kRunTimeMod)
        return acEntry.Data.Poison.IsSet() ? Make(acEntry.Data.Poison) : nullptr;
    return Cast<AlchemyItem>(TESForm::GetById(World::Get().GetModSystem().GetGameId(acEntry.ExtraPoisonId)));
}

void SetName(ExtraDataList* apExtraDataList, const TiltedPhoques::String& acName) noexcept
{
    if (!apExtraDataList || acName.empty())
        return;

    auto* pText = Find<ExtraTextDisplayData>(apExtraDataList);
    // A quest's name for its item stays.
    if (pText && pText->pDisplayNameText)
        return;

    if (!pText)
    {
        POINTER_SKYRIMSE(void*, s_vtbl, 186855, 229625);
        pText = Memory::Allocate<ExtraTextDisplayData>();
        std::memset(static_cast<void*>(pText), 0, sizeof(ExtraTextDisplayData));
        *reinterpret_cast<void**>(pText) = s_vtbl.Get();
        if (!apExtraDataList->Add(ExtraDataType::TextDisplayData, pText))
        {
            Memory::Free(pText);
            return;
        }
    }

    pText->DisplayName.Set(acName.c_str());
    pText->usCustomNameLength = static_cast<uint16_t>(acName.size());
    pText->iOwnerInstance = kCustomName;
    pText->fTemperFactor = 1.f;
}

void Update(ExtraDataList* apExtraDataList, const Inventory::Entry& acNew) noexcept
{
    if (!apExtraDataList)
        return;

    if (acNew.ExtraHealth > 0.f)
    {
        if (auto* pHealth = Find<ExtraHealth>(apExtraDataList))
            pHealth->fHealth = acNew.ExtraHealth;
        else
            apExtraDataList->SetHealth(acNew.ExtraHealth);
    }
    else
        Drop(apExtraDataList, ExtraDataType::Health);

    if (auto* pCharge = Find<ExtraCharge>(apExtraDataList))
        pCharge->fCharge = acNew.ExtraCharge;
    else if (acNew.ExtraCharge > 0.f)
        apExtraDataList->SetChargeData(acNew.ExtraCharge);

    // Another poison or enchantment is taken off and put on as the game does: it may count who holds a made one.
    AlchemyItem* pPoison = GetPoison(acNew);
    auto* pExtraPoison = Find<ExtraPoison>(apExtraDataList);
    if (pPoison && acNew.ExtraPoisonCount > 0)
    {
        if (pExtraPoison && pExtraPoison->pPoison == pPoison)
            pExtraPoison->uiCount = acNew.ExtraPoisonCount;
        else
        {
            Drop(apExtraDataList, ExtraDataType::Poison);
            apExtraDataList->SetPoison(pPoison, acNew.ExtraPoisonCount);
        }
    }
    else if (pExtraPoison)
        Drop(apExtraDataList, ExtraDataType::Poison);

    // The enchantment as the stack has it now, by the id it is sent by.
    Inventory::Entry now{};
    now.BaseId = acNew.BaseId;
    {
        ScopedExtraDataOverride _;
        TESObjectREFR::GetItemFromExtraData(now, apExtraDataList);
    }
    if (now.ExtraEnchantId != acNew.ExtraEnchantId || now.ExtraEnchantCharge != acNew.ExtraEnchantCharge || now.ExtraEnchantRemoveUnequip != acNew.ExtraEnchantRemoveUnequip)
    {
        Drop(apExtraDataList, ExtraDataType::Enchantment);
        if (EnchantmentItem* pEnchantment = GetEnchantment(acNew))
            apExtraDataList->SetEnchantmentData(pEnchantment, acNew.ExtraEnchantCharge, acNew.ExtraEnchantRemoveUnequip);
    }

    if (auto* pSoul = Find<ExtraSoul>(apExtraDataList))
    {
        if (acNew.ExtraSoulLevel > 0 && acNew.ExtraSoulLevel <= 5)
            pSoul->cSoul = static_cast<SOUL_LEVEL>(acNew.ExtraSoulLevel);
        else
            Drop(apExtraDataList, ExtraDataType::Soul);
    }
    else if (acNew.ExtraSoulLevel > 0 && acNew.ExtraSoulLevel <= 5)
        apExtraDataList->SetSoulData(static_cast<SOUL_LEVEL>(acNew.ExtraSoulLevel));

    if (!acNew.Data.DisplayName.empty())
        SetName(apExtraDataList, acNew.Data.DisplayName);
    else if (FindCustomName(apExtraDataList))
        Drop(apExtraDataList, ExtraDataType::TextDisplayData);
}

ExtraDataList* FindStack(TESObjectREFR* apOwner, const TESForm* apItem, const Inventory::Entry& acEntry) noexcept
{
    const Inventory::Entry cWanted = ItemDataChanges::Stack(acEntry);
    auto* pChanges = apOwner && apItem && cWanted.ContainsExtraData() ? apOwner->GetContainerChanges() : nullptr;
    if (!pChanges || !pChanges->entries)
        return nullptr;

    // One already in that hand first, then one in no hand; one in the other hand only when there is no other,
    // or two equal daggers would leave one hand empty.
    const bool cLeft = acEntry.ExtraWornLeft;
    ExtraDataList* pFree = nullptr;
    ExtraDataList* pOtherHand = nullptr;

    for (ExtraContainerChanges::Entry* pEntry : *pChanges->entries)
    {
        if (!pEntry || pEntry->form != apItem || !pEntry->dataList)
            continue;
        for (ExtraDataList* pList : *pEntry->dataList)
        {
            if (!pList)
                continue;
            Inventory::Entry stack{};
            stack.BaseId = acEntry.BaseId;
            stack.Data.Base = acEntry.Data.Base;
            {
                ScopedExtraDataOverride _;
                TESObjectREFR::GetItemFromExtraData(stack, pList);
            }
            if (!ItemDataChanges::Stack(stack).CanBeMerged(cWanted))
                continue;
            const bool cInLeft = pList->Contains(ExtraDataType::WornLeft);
            const bool cInRight = pList->Contains(ExtraDataType::Worn);
            if (cLeft ? cInLeft : cInRight)
                return pList;
            if (!cInLeft && !cInRight)
                pFree = pFree ? pFree : pList;
            else
                pOtherHand = pOtherHand ? pOtherHand : pList;
        }
    }
    return pFree ? pFree : pOtherHand;
}

int32_t Apply(TESObjectREFR* apOwner, const ItemDataChange& acChange) noexcept
{
    if (!apOwner || acChange.Old.Count <= 0 || acChange.Old.BaseId != acChange.New.BaseId)
        return 0;
    TESBoundObject* pObject = GetBase(acChange.Old);
    auto* pChanges = pObject ? apOwner->GetContainerChanges() : nullptr;
    if (!pChanges || !pChanges->entries)
        return 0;

    const Inventory::Entry cOld = ItemDataChanges::Stack(acChange.Old);
    int32_t left = acChange.Old.Count;
    // A stack of more items than changed gives the changed ones up: split off after the walk, which must not change the list.
    ExtraDataList* pSplit = nullptr;
    const ExtraContainerChanges::Entry* pItemEntry = nullptr;
    for (ExtraContainerChanges::Entry* pEntry : *pChanges->entries)
    {
        if (!pEntry || pEntry->form != pObject || !pEntry->dataList)
            continue;
        pItemEntry = pEntry;
        for (ExtraDataList* pList : *pEntry->dataList)
        {
            if (left <= 0 || pSplit || !pList)
                continue;
            Inventory::Entry stack{};
            stack.BaseId = acChange.Old.BaseId;
            stack.Data.Base = acChange.Old.Data.Base;
            stack.Count = 1;
            {
                ScopedExtraDataOverride _;
                TESObjectREFR::GetItemFromExtraData(stack, pList);
            }
            if (!ItemDataChanges::Stack(stack).CanBeMerged(cOld))
                continue;
            if (stack.Count > left)
            {
                pSplit = pList;
                continue;
            }
            Update(pList, acChange.New);
            left -= stack.Count;
        }
    }

    // Plain items have no extra data of their own: the changed ones, like a split stack's, are taken out and given back with the new data.
    if (left > 0 && (pSplit || (!cOld.ContainsExtraData() && PlainCount(apOwner, pObject, pItemEntry) >= left)))
    {
        ScopedInventoryOverride _;
        Inventory::Entry added = ItemDataChanges::Stack(acChange.New);
        added.Count = left;
        apOwner->RemoveItem(pObject, left, ITEM_REMOVE_REASON::kRemove, pSplit, nullptr);
        apOwner->AddObjectToContainer(pObject, TESObjectREFR::GetExtraDataFromItem(added), left, nullptr);
        left = 0;
    }

    return acChange.Old.Count - left;
}
} // namespace ItemExtraData
