#include <TiltedOnlinePCH.h>

#include <Services/Generic/CopyLootCheck.h>

#include <World.h>
#include <Actor.h>
#include <Games/ActorExtension.h>
#include <ExtraData/ExtraContainerChanges.h>
#include <ExtraData/ExtraDataList.h>
#include <LootKinds.h>

namespace
{
struct PendingCheck
{
    uint32_t FormId{};
    const Actor* pActor{};
    Inventory Owner{};
    std::chrono::steady_clock::time_point Due{};
    uint32_t SetsLeft{};
};
TiltedPhoques::Vector<PendingCheck> s_pending;
// A set made by a check is not a new one to check from the start.
bool s_setting = false;

constexpr std::chrono::seconds cFirstCheck{2};
constexpr std::chrono::seconds cSecondCheck{4};
constexpr uint32_t cSets = 2;

bool IsItem(const GameId& acId) noexcept
{
    const TESForm* pForm = TESForm::GetById(World::Get().GetModSystem().GetGameId(acId));
    return pForm && pForm->formType != FormType::LeveledItem;
}

// Whether the game marked any of the copy's lists of that item as its own outfit's.
bool IsOutfitItem(const Actor* apActor, const GameId& acId) noexcept
{
    const uint32_t cItemId = World::Get().GetModSystem().GetGameId(acId);
    const ExtraContainerChanges::Data* pChanges = apActor->GetContainerChanges();
    if (!pChanges || !pChanges->entries)
        return false;
    for (const ExtraContainerChanges::Entry* pEntry : *pChanges->entries)
    {
        if (!pEntry || !pEntry->form || pEntry->form->formID != cItemId || !pEntry->dataList)
            continue;
        for (ExtraDataList* pList : *pEntry->dataList)
            if (pList && pList->Contains(ExtraDataType::OutfitItem))
                return true;
    }
    return false;
}

std::string Kinds(const Vector<GameId>& acIds) noexcept
{
    std::string text;
    for (const GameId& id : acIds)
    {
        if (!text.empty())
            text += ", ";
        text += fmt::format("{:X}", World::Get().GetModSystem().GetGameId(id));
    }
    return text.empty() ? "none" : text;
}
} // namespace

namespace CopyLootCheck
{
void Note(Actor* apActor, const Inventory& acOwnerInventory) noexcept
{
    if (s_setting || !apActor)
        return;
    const ActorExtension* pExtension = apActor->GetExtension();
    if (!pExtension || !pExtension->IsRemote() || pExtension->IsRemotePlayer())
        return;

    const uint32_t cFormId = apActor->formID;
    s_pending.erase(std::remove_if(s_pending.begin(), s_pending.end(), [cFormId](const PendingCheck& acCheck) { return acCheck.FormId == cFormId; }), s_pending.end());
    // A bound for a battle of many copies arriving at once: the oldest checks go first.
    if (s_pending.size() >= 512)
        s_pending.erase(s_pending.begin());
    s_pending.push_back({cFormId, apActor, acOwnerInventory, std::chrono::steady_clock::now() + cFirstCheck, cSets});
}

void OnOwnerChange(const uint32_t aFormId, const Inventory::Entry& acItem, const bool aDrop) noexcept
{
    for (auto& check : s_pending)
    {
        if (check.FormId != aFormId)
            continue;
        // As the copy has it applied: a drop takes the items out, a pick-up does nothing yet.
        if (!aDrop || acItem.Count < 0)
            check.Owner.AddOrRemoveEntry(acItem);
        return;
    }
}

void OnOwnerEquipment(const uint32_t aFormId, const Inventory& acOwnerEquipment) noexcept
{
    for (auto& check : s_pending)
    {
        if (check.FormId != aFormId)
            continue;
        check.Owner.UpdateEquipment(acOwnerEquipment);
        return;
    }
}

void Run() noexcept
{
    if (s_pending.empty())
        return;

    const auto cNow = std::chrono::steady_clock::now();
    for (auto it = s_pending.begin(); it != s_pending.end();)
    {
        if (cNow < it->Due)
        {
            ++it;
            continue;
        }

        Actor* pActor = Cast<Actor>(TESForm::GetById(it->FormId));
        const ActorExtension* pExtension = pActor ? pActor->GetExtension() : nullptr;
        // Gone, another actor under its id, or no longer a copy: nothing of its owner's to hold.
        // A body is left alone too: what a player takes from it here is no difference to undo.
        if (!pActor || pActor != it->pActor || pActor->IsDeleted() || pActor->IsDead() || !pExtension || !pExtension->IsRemote() || !pActor->HasLoadedProcess())
        {
            it = s_pending.erase(it);
            continue;
        }

        const LootKinds::Difference cDifference = LootKinds::Compare(it->Owner, pActor->GetActorInventory(), IsItem);
        if (cDifference.IsEmpty())
        {
            it = s_pending.erase(it);
            continue;
        }

        const auto cOutfit = std::count_if(cDifference.Extra.begin(), cDifference.Extra.end(), [pActor](const GameId& acId) { return IsOutfitItem(pActor, acId); });
        if (it->SetsLeft == 0)
        {
            spdlog::debug(
                "CopyLoot: copy {:X} still holds kinds its owner does not ({}; {} of the game's outfit) and lacks {} after {} sets; kept", it->FormId, Kinds(cDifference.Extra),
                cOutfit, Kinds(cDifference.Missing), cSets);
            it = s_pending.erase(it);
            continue;
        }

        spdlog::debug(
            "CopyLoot: copy {:X} holds kinds its owner does not ({}; {} of the game's outfit) and lacks {}; its owner's items set again", it->FormId, Kinds(cDifference.Extra),
            cOutfit, Kinds(cDifference.Missing));
        --it->SetsLeft;
        it->Due = cNow + cSecondCheck;
        s_setting = true;
        pActor->SetActorInventory(it->Owner);
        s_setting = false;
        ++it;
    }
}
} // namespace CopyLootCheck
