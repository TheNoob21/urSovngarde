#include <TiltedOnlinePCH.h>

#define UR_GAME_HOST
#include <GameApi.h>
#include <FollowerPolicy.h>
#include <NetImmerse/NiTransform.h>
#include <Services/GameExtension.h>
#include <Services/RagdollPoseService.h>
#include <Services/HitReactionService.h>
#include <Services/VRSwingService.h>
#include <Components.h>
#include <World.h>
#include <Structs/GameId.h>
#include <Utils.h>
#include <Games/TES.h>
#include <Games/ActorExtension.h>
#include <Actor.h>
#include <PlayerCharacter.h>
#include <EquipManager.h>
#include <Forms/TESObjectARMO.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Services/TransportService.h>
#include <Services/PapyrusService.h>
#include <Services/CopyWorkGate.h>
#include <SaveLoad.h>
#include <Services/VoiceService.h>
#include <Games/Misc/SubtitleManager.h>
#include <Forms/TESGlobal.h>
#include <Forms/TESIdleForm.h>
#include <Messages/RequestOwnershipClaim.h>
#include <Games/Skyrim/VR/VRBodySync.h>

namespace
{
struct Entry
{
    uint32_t Id;
    UrGameHooks Hooks;
    std::atomic<uint32_t (*)(uint32_t)> Reserved, BeforeDelete;
    std::atomic<void (*)()> Step;
    std::atomic<uint32_t (*)(uint32_t, uint32_t, uint64_t, UrGamePose*)> Pose;
};
std::array<Entry, 32> s_entries{};
std::mutex s_register;
std::atomic<size_t> s_count{};

uint32_t Register(uint32_t aId, const UrGameHooks* apHooks)
{
    if (!aId || !apHooks)
        return 0;
    std::scoped_lock lock(s_register);
    const size_t cCount = s_count.load();
    if (cCount == s_entries.size())
        return 0;
    for (size_t i = 0; i < cCount; ++i)
        if (s_entries[i].Id == aId)
            return 0;
    s_entries[cCount].Id = aId;
    s_entries[cCount].Hooks = *apHooks;
    s_count.store(cCount + 1);
    return 1;
}

uint32_t Copy(std::string_view aText, char* apOut, uint32_t aCapacity)
{
    if (!apOut || !aCapacity || aText.empty() || aText.size() >= aCapacity)
        return 0;
    std::memcpy(apOut, aText.data(), aText.size());
    apOut[aText.size()] = 0;
    return 1;
}

uint32_t Endpoint(char* apOut, uint32_t aCapacity)
{
    return entt::locator<World>::has_value() ? Copy(World::Get().GetTransport().GetServerEndpoint(), apOut, aCapacity) : 0;
}

uint32_t PluginForm(uint32_t aFormId, char* apOut, uint32_t aCapacity, uint32_t* apBase)
{
    auto* pManager = ModManager::Get();
    if (!pManager || !aFormId || !apBase || TESForm::IsTemporaryId(aFormId))
        return 0;
    for (auto* pEntry = &pManager->mods.entry; pEntry && pEntry->data; pEntry = pEntry->next)
    {
        const auto* pMod = pEntry->data;
        if (pMod->IsLoaded() && pMod->GetFormId(aFormId) == aFormId)
        {
            *apBase = aFormId & (pMod->IsLite() ? 0xfff : 0xffffff);
            return Copy(pMod->filename, apOut, aCapacity);
        }
    }
    return 0;
}

const void* Papyrus(const char* apClass, const char* apName)
{
    return apClass && apName && entt::locator<World>::has_value() ? World::Get().ctx().at<PapyrusService>().Get(apClass, apName) : nullptr;
}

void* VirtualMachine()
{
    auto* pVm = GameVM::Get();
    return pVm ? pVm->virtualMachine : nullptr;
}

uint32_t ReadActor(uint32_t aId, UrGameActor* apOut)
{
    if (!entt::locator<World>::has_value())
        return 0;
    const auto cEntity = Utils::FindEntityByServerId(aId);
    if (!cEntity || !apOut)
        return 0;
    auto& world = World::Get();
    const auto* pForm = world.try_get<FormIdComponent>(*cEntity);
    const auto* pLocal = world.try_get<LocalComponent>(*cEntity);
    const auto* pRemote = world.try_get<RemoteComponent>(*cEntity);
    if (!pForm || (!pLocal && !pRemote))
        return 0;
    auto* pActor = Cast<Actor>(TESForm::GetById(pForm->Id));
    if (!pActor || pActor->IsDeleted())
        return 0;
    auto* pCell = pActor->GetParentCellEx();
    const auto* pPlayer = world.try_get<PlayerComponent>(*cEntity);
    static const PapyrusLookup s_furniture("Actor", "GetFurnitureReference");
    using Furniture = TESObjectREFR*(__fastcall*)(void*, uint32_t, Actor*);
    const auto cReadSeat = reinterpret_cast<Furniture>(const_cast<void*>(s_furniture.Resolve()));
    auto* pVm = VirtualMachine();
    *apOut = {};
    apOut->Form = pForm->Id;
    apOut->Id = aId;
    apOut->Epoch = pLocal ? pLocal->OwnershipEpoch : pRemote->OwnershipEpoch;
    apOut->Owned = pLocal != nullptr;
    apOut->Player = pPlayer ? pPlayer->Id : (pActor == PlayerCharacter::Get() ? world.GetTransport().GetLocalPlayerId() : 0);
    apOut->Dead = pActor->IsDead() || (pLocal ? pLocal->IsDead : pRemote->DeadAtOwner || pRemote->DeadHere);
    apOut->Mounted = !Actor::CanReadMount() || pActor->IsOnMount();
    apOut->Sitting = !cReadSeat || !pVm || cReadSeat(pVm, 0, pActor) != nullptr;
    apOut->Follower = pActor->IsPlayerFollower();
    apOut->Loaded = pActor->HasLoadedProcess() && pActor->GetNiNode();
    apOut->Cell = pCell ? pCell->formID : 0;
    apOut->World = pCell && pCell->worldspace ? pCell->worldspace->formID : 0;
    apOut->Position[0] = pActor->position.x;
    apOut->Position[1] = pActor->position.y;
    apOut->Position[2] = pActor->position.z;
    return 1;
}

void VisitActors(void* apContext, void (*apVisit)(void*, const UrGameActor*))
{
    if (!apVisit || !entt::locator<World>::has_value())
        return;
    auto& world = World::Get();
    std::vector<uint32_t> ids;
    for (const auto cEntity : world.view<LocalComponent>())
        ids.push_back(world.get<LocalComponent>(cEntity).Id);
    for (const auto cEntity : world.view<RemoteComponent>())
        ids.push_back(world.get<RemoteComponent>(cEntity).Id);
    for (const auto cId : ids)
    {
        UrGameActor actor{};
        if (ReadActor(cId, &actor))
            apVisit(apContext, &actor);
    }
}

uint32_t WornArmor(uint32_t aActorForm, uint32_t aSlot, uint32_t* apForms, uint32_t aCapacity)
{
    auto* pActor = Cast<Actor>(TESForm::GetById(aActorForm));
    if (!pActor || !pActor->HasLoadedProcess() || pActor->IsDeleted() || !aSlot || !entt::locator<World>::has_value())
        return UINT32_MAX;
    const auto& cMods = World::Get().GetModSystem();
    uint32_t count{};
    for (const auto& cEntry : pActor->GetWornArmor().Entries)
    {
        if (!cEntry.IsWorn() || cEntry.Count <= 0)
            continue;
        const auto* pArmor = Cast<TESObjectARMO>(TESForm::GetById(cMods.GetGameId(cEntry.BaseId)));
        if (pArmor && pArmor->slotType == aSlot)
        {
            if (apForms && count < aCapacity)
                apForms[count] = pArmor->formID;
            ++count;
        }
    }
    return count;
}

uint32_t EquipCopyArmor(uint32_t aActorForm, uint32_t aItem, uint32_t aSlot, uint32_t aEquip)
{
    auto* pActor = Cast<Actor>(TESForm::GetById(aActorForm));
    auto* pItem = Cast<TESObjectARMO>(TESForm::GetById(aItem));
    auto* pEquip = EquipManager::Get();
    if (!pActor || !pItem || pItem->slotType != aSlot || !pEquip || !pActor->HasLoadedProcess() || !pActor->GetNiNode() || pActor->IsDeleted() || pActor->IsPlayerFollower() ||
        !pActor->GetExtension()->IsRemote())
        return 0;
    if (aEquip)
    {
        if (pActor->GetItemCountInInventory(pItem) <= 0)
            return 0;
        pEquip->Equip(pActor, pItem, nullptr, 1, nullptr, false, true, false, false);
    }
    else
        pEquip->UnEquip(pActor, pItem, nullptr, 1, nullptr, false, true, false, false, nullptr);
    return 1;
}

uint32_t Describe(TESObjectREFR* apRef, UrGameReference* apOut)
{
    if (!apRef || !apOut || !apRef->baseForm)
        return 0;
    auto* pCell = apRef->GetParentCellEx();
    *apOut = {};
    apOut->Handle = apRef->GetHandle().handle.iBits;
    apOut->Form = apRef->formID;
    apOut->Base = apRef->baseForm->formID;
    apOut->BaseType = static_cast<uint32_t>(apRef->baseForm->formType);
    apOut->Cell = pCell ? pCell->formID : 0;
    apOut->World = pCell && pCell->worldspace ? pCell->worldspace->formID : 0;
    apOut->Disabled = apRef->IsDisabled();
    apOut->Deleted = apRef->IsDeleted();
    apOut->Aliased = apRef->extraData.Contains(ExtraDataType::AliasInstanceArray);
    apOut->Scale = apRef->scale / 100.f;
    apOut->Position[0] = apRef->position.x;
    apOut->Position[1] = apRef->position.y;
    apOut->Position[2] = apRef->position.z;
    apOut->Rotation[0] = apRef->rotation.x;
    apOut->Rotation[1] = apRef->rotation.y;
    apOut->Rotation[2] = apRef->rotation.z;
    return 1;
}

uint32_t Reference(uint32_t aForm, uint32_t aHandle, UrGameReference* apOut)
{
    const RetainedReference cHeld(aHandle);
    auto* pRef = aHandle ? cHeld.Get() : Cast<TESObjectREFR>(TESForm::GetById(aForm));
    return pRef && (!aForm || pRef->formID == aForm) && Describe(pRef, apOut);
}

TESObjectCELL* Cell(const UrGameReference& acState)
{
    auto* pForm = TESForm::GetById(acState.Cell);
    if (!pForm || pForm->IsTemporary() || static_cast<uint32_t>(pForm->formType) != 60)
        return nullptr;
    auto* pCell = static_cast<TESObjectCELL*>(pForm);
    return pCell->IsAttached() && (pCell->worldspace ? pCell->worldspace->formID : 0) == acState.World ? pCell : nullptr;
}

uint32_t RemoveCopy(uint32_t aForm, uint32_t aHandle)
{
    const RetainedReference cHeld(aHandle);
    auto* pRef = cHeld.Get();
    if (!pRef || !pRef->IsTemporary() || (aForm && pRef->formID != aForm) || pRef->extraData.Contains(ExtraDataType::AliasInstanceArray))
        return 0;
    pRef->Disable();
    pRef->Delete();
    return 1;
}

bool ValidTransform(const UrGameReference* apState)
{
    if (!apState || !std::isfinite(apState->Scale) || apState->Scale <= 0 || apState->Scale > 64)
        return false;
    for (size_t i = 0; i < 3; ++i)
        if (!std::isfinite(apState->Position[i]) || !std::isfinite(apState->Rotation[i]) || std::abs(apState->Position[i]) > 16777216.f)
            return false;
    return true;
}

uint32_t ApplyReference(const UrGameReference* apState)
{
    if (!ValidTransform(apState))
        return 0;
    const RetainedReference cHeld(apState->Handle);
    auto* pRef = cHeld.Get();
    auto* pCell = Cell(*apState);
    if (!pRef || !pCell || !pRef->IsTemporary() || !pRef->baseForm || apState->Base != pRef->baseForm->formID || (apState->Form && apState->Form != pRef->formID) ||
        pRef->extraData.Contains(ExtraDataType::AliasInstanceArray))
        return 0;
    if (apState->Deleted)
        return RemoveCopy(pRef->formID, apState->Handle);
    const NiPoint3 cPos{glm::vec3(apState->Position[0], apState->Position[1], apState->Position[2])};
    if (pRef->GetParentCellEx() == pCell)
        pRef->SetPositionInPlace(cPos);
    else
        pRef->MoveTo(pCell, cPos);
    if (pRef->IsDeleted())
        return 0;
    pRef->SetRotation(apState->Rotation[0], apState->Rotation[1], apState->Rotation[2]);
    if (std::abs(pRef->scale / 100.f - apState->Scale) > .005f)
    {
        using ObjectReference = TESObjectREFR;
        PAPYRUS_FUNCTION(void, ObjectReference, SetScale, float);
        s_pSetScale(pRef, apState->Scale);
    }
    if (apState->Disabled && !pRef->IsDisabled())
        pRef->Disable();
    if (!apState->Disabled && pRef->IsDisabled())
        pRef->EnableImpl();
    return 1;
}

uint32_t CreateCopy(const UrGameReference* apState, uint32_t aInitiallyDisabled, uint32_t aBlockActivation)
{
    if (!ValidTransform(apState) || apState->Deleted)
        return 0;
    auto* pCell = Cell(*apState);
    auto* pBase = TESForm::GetById(apState->Base);
    if (!pCell || !pBase || pBase->IsTemporary())
        return 0;
    using ObjectReference = TESObjectREFR;
    uint32_t handle{};
    if (aInitiallyDisabled)
    {
        PAPYRUS_FUNCTION(TESObjectREFR*, ObjectReference, PlaceAtMe, TESForm*, int32_t, bool, bool);
        if (auto* pAnchor = PlayerCharacter::Get())
            if (auto* pRef = s_pPlaceAtMe(pAnchor, pBase, 1, false, true))
                handle = pRef->GetHandle().handle.iBits;
    }
    else
    {
        const auto* pPosition = apState->Position;
        const auto* pRotation = apState->Rotation;
        NiPoint3 pos{glm::vec3(pPosition[0], pPosition[1], pPosition[2])}, rot{glm::vec3(pRotation[0], pRotation[1], pRotation[2])};
        handle = ModManager::Get()->SpawnReference(pBase, pos, rot, pCell, pCell->worldspace);
    }
    const RetainedReference cHeld(handle);
    auto* pRef = cHeld.Get();
    if (!pRef || !pRef->IsTemporary() || pRef->baseForm != pBase || pRef->extraData.Contains(ExtraDataType::AliasInstanceArray))
        return 0;
    pRef->SetSkipSaveFlag(true);
    if (aBlockActivation)
    {
        PAPYRUS_FUNCTION(void, ObjectReference, BlockActivation, bool);
        s_pBlockActivation(pRef, true);
        if (!pRef->IsActivationBlocked())
        {
            RemoveCopy(pRef->formID, handle);
            return 0;
        }
    }
    auto state = *apState;
    state.Handle = handle;
    state.Form = pRef->formID;
    if (!ApplyReference(&state))
    {
        RemoveCopy(pRef->formID, handle);
        return 0;
    }
    return handle;
}

uint64_t SaveGeneration()
{
    const auto cState = PlayerSaveContext::Read();
    return cState.Loading || !PlayerCharacter::Get() ? 0 : uint64_t(cState.Generation) + 1;
}
uint32_t WithSave(uint64_t aGeneration, void* apContext, void (*apWork)(void*))
{
    const auto cState = PlayerSaveContext::Read();
    return apWork && aGeneration && !cState.Loading && PlayerCharacter::Get() && uint64_t(cState.Generation) + 1 == aGeneration &&
           PlayerSaveContext::RunFor(cState, [&]() { apWork(apContext); });
}
struct PresentationEntry
{
    uint32_t Addon;
    UrPresentNode Present;
};
std::array<PresentationEntry, 32> s_presenters{};
std::atomic<size_t> s_presenterCount{};

uint32_t RegisterPresentation(uint32_t aAddon, UrPresentNode aPresent)
{
    if (!aAddon || !aPresent)
        return 0;
    std::scoped_lock lock(s_register);
    const auto cCount = s_presenterCount.load();
    if (cCount >= s_presenters.size())
        return 0;
    for (size_t i = 0; i < cCount; ++i)
        if (s_presenters[i].Addon == aAddon)
            return 0;
    s_presenters[cCount] = {aAddon, aPresent};
    s_presenterCount.store(cCount + 1);
    return 1;
}

uint32_t RegisterPose(uint32_t aId, uint32_t (*apPose)(uint32_t, uint32_t, uint64_t, UrGamePose*))
{
    std::scoped_lock lock(s_register);
    for (size_t i = 0; i < s_count.load(); ++i)
        if (s_entries[i].Id == aId && apPose && !s_entries[i].Pose.load())
        {
            s_entries[i].Pose.store(apPose);
            return 1;
        }
    return 0;
}
std::array<std::pair<uint32_t, UrProjectileLaunched>, 32> s_projectiles{};
std::atomic<size_t> s_projectileCount{};

uint32_t RegisterProjectile(uint32_t aId, UrProjectileLaunched aObserver)
{
    if (!aId || !aObserver)
        return 0;
    std::scoped_lock lock(s_register);
    const auto cCount = s_projectileCount.load();
    if (cCount >= s_projectiles.size())
        return 0;
    for (size_t i = 0; i < cCount; ++i)
        if (s_projectiles[i].first == aId)
            return 0;
    s_projectiles[cCount] = {aId, aObserver};
    s_projectileCount.store(cCount + 1);
    return 1;
}

void Speech(uint32_t aActor, const int16_t* apSamples, uint32_t aCount)
{
    if (apSamples && aCount && aCount <= 2048 && entt::locator<World>::has_value())
        World::Get().ctx().at<VoiceService>().PlayNpcSpeech(aActor, 16000, std::vector<int16_t>(apSamples, apSamples + aCount));
}
void StopSpeech(uint32_t aActor)
{
    if (entt::locator<World>::has_value())
        World::Get().ctx().at<VoiceService>().StopNpcSpeech(aActor);
}
void Subtitle(uint32_t aId, const char* apText)
{
    auto* pActor = Utils::GetByServerId<Actor>(aId);
    auto* pSubtitles = SubtitleManager::Get();
    if (!pActor || !pSubtitles)
        return;
    if (apText && *apText)
        pSubtitles->ShowSubtitle(pActor, apText, nullptr);
    else
        pSubtitles->HideSubtitle(pActor);
}
uint32_t MoveReference(const UrGameReference* apState, uint32_t aAllowDetached)
{
    if (!ValidTransform(apState) || !apState->Handle || !apState->Form || !apState->Cell)
        return 0;
    const RetainedReference cHeld(apState->Handle);
    auto* pRef = cHeld.Get();
    auto* pForm = TESForm::GetById(apState->Cell);
    if (!pRef || pRef->formID != apState->Form || !pRef->baseForm || pRef->baseForm->formID != apState->Base || !pForm || static_cast<uint32_t>(pForm->formType) != 60 ||
        pRef->IsDeleted() || !pRef->GetNiNode())
        return 0;
    auto* pCell = static_cast<TESObjectCELL*>(pForm);
    if ((!aAllowDetached && !pCell->IsAttached()) || (pCell->worldspace ? pCell->worldspace->formID : 0) != apState->World)
        return 0;
    for (size_t i = 0; i < 3; ++i)
        if (!std::isfinite(apState->Position[i]) || std::abs(apState->Position[i]) > 2.e7f || !std::isfinite(apState->Rotation[i]))
            return 0;
    const NiPoint3 cPos{glm::vec3(apState->Position[0], apState->Position[1], apState->Position[2])};
    if (pRef->GetParentCellEx() != pCell)
        pRef->MoveTo(pCell, cPos);
    else
        pRef->SetPositionInPlace(cPos);
    pRef->SetRotation(apState->Rotation[0], apState->Rotation[1], apState->Rotation[2]);
    return 1;
}
uint32_t Motion(uint32_t aForm, uint32_t aHandle, uint32_t aMotion)
{
    const RetainedReference cHeld(aHandle);
    auto* pRef = cHeld.Get();
    if (!pRef || pRef->formID != aForm || pRef->IsDeleted() || !pRef->GetNiNode() || (aMotion != 1 && aMotion != 4))
        return 0;
    pRef->SetMotionType(static_cast<int32_t>(aMotion));
    return 1;
}
uint32_t Vehicle(uint32_t aForm, uint32_t aHandle, uint32_t aVehicleForm, uint32_t aIdleForm)
{
    const RetainedReference cHeld(aHandle);
    auto* pActor = Cast<Actor>(cHeld.Get());
    auto* pVehicle = aVehicleForm ? Cast<TESObjectREFR>(TESForm::GetById(aVehicleForm)) : nullptr;
    auto* pIdle = Cast<TESIdleForm>(TESForm::GetById(aIdleForm));
    if (!pActor || pActor->formID != aForm || pActor->IsDeleted() || !pActor->HasLoadedProcess() || !pActor->GetNiNode() ||
        (aVehicleForm && (!pVehicle || !pVehicle->GetNiNode() || !GameExtension::ReleaseFurnitureSeat(pActor))) || !pIdle)
        return 0;
    pActor->SetVehicle(pVehicle);
    return pActor->PlayIdle(pIdle);
}
uint32_t GlobalValue(uint32_t aForm, float* apValue, uint32_t aWrite)
{
    auto* pGlobal = Cast<TESGlobal>(TESForm::GetById(aForm));
    if (!pGlobal || !apValue || (aWrite && !std::isfinite(*apValue)))
        return 0;
    if (aWrite)
        pGlobal->f = *apValue;
    else
        *apValue = pGlobal->f;
    return 1;
}
uint32_t ClaimMount(uint32_t aId, uint32_t aEpoch)
{
    if (!entt::locator<World>::has_value())
        return 0;
    RequestOwnershipClaim claim;
    claim.ServerId = aId;
    claim.ExpectedOwnershipEpoch = aEpoch;
    claim.ForRiding = true;
    return World::Get().GetTransport().Send(claim);
}
uint32_t Interaction(uint32_t aId, uint32_t (*apReserved)(uint32_t), uint32_t (*apBeforeDelete)(uint32_t), void (*apStep)())
{
    std::scoped_lock guard(s_register);
    for (size_t i = 0; i < s_count.load(); ++i)
        if (s_entries[i].Id == aId && !s_entries[i].Step.load() && apStep)
        {
            s_entries[i].Reserved.store(apReserved);
            s_entries[i].BeforeDelete.store(apBeforeDelete);
            s_entries[i].Step.store(apStep);
            return 1;
        }
    return 0;
}
std::array<std::pair<uint32_t, UrCapturesRagdoll>, 32> s_ragdolls{};
std::atomic<size_t> s_ragdollCount{};
uint32_t RegisterRagdoll(uint32_t aId, UrCapturesRagdoll aCaptures)
{
    if (!aId || !aCaptures)
        return 0;
    std::scoped_lock lock(s_register);
    const auto cCount = s_ragdollCount.load();
    if (cCount >= s_ragdolls.size())
        return 0;
    for (size_t i = 0; i < cCount; ++i)
        if (s_ragdolls[i].first == aId)
            return 0;
    s_ragdolls[cCount] = {aId, aCaptures};
    s_ragdollCount.store(cCount + 1);
    return 1;
}
void VisitCell(const uint8_t* apTypes, uint32_t aCount, void* apContext, void (*apVisit)(void*, const UrGameReference*))
{
    auto* pPlayer = PlayerCharacter::Get();
    auto* pCell = pPlayer ? pPlayer->GetParentCellEx() : nullptr;
    if (!pCell || !pCell->IsAttached() || !apTypes || !aCount || aCount > 32 || !apVisit)
        return;
    Vector<FormType> kinds;
    for (uint32_t i = 0; i < aCount; ++i)
        kinds.push_back(static_cast<FormType>(apTypes[i]));
    for (auto* pRef : pCell->GetRefsByFormTypes(kinds))
    {
        UrGameReference state{};
        if (!pRef || pRef->IsDeleted() || pRef->IsDisabled() || !pRef->GetNiNode() || !Describe(pRef, &state) || state.Cell != pCell->formID)
            continue;
        const auto& cPosition = WorldTransformOf(pRef->GetNiNode()).translate;
        state.Position[0] = cPosition.x;
        state.Position[1] = cPosition.y;
        state.Position[2] = cPosition.z;
        apVisit(apContext, &state);
    }
}
std::array<std::pair<uint32_t, UrTakeObjects>, 32> s_objects{};
std::atomic<size_t> s_objectCount{};
uint32_t RegisterObjects(uint32_t aId, UrTakeObjects aTake)
{
    if (!aId || !aTake)
        return 0;
    std::scoped_lock lock(s_register);
    const auto cCount = s_objectCount.load();
    if (cCount >= s_objects.size())
        return 0;
    for (size_t i = 0; i < cCount; ++i)
        if (s_objects[i].first == aId)
            return 0;
    s_objects[cCount] = {aId, aTake};
    s_objectCount.store(cCount + 1);
    return 1;
}
std::array<std::pair<uint32_t, UrGameReactionHooks>, 32> s_reactions{};
std::atomic<size_t> s_reactionCount{};
uint32_t RegisterReactions(uint32_t aId, const UrGameReactionHooks* apHooks)
{
    if (!aId || !apHooks || !apHooks->Unpaired || !apHooks->Graph)
        return 0;
    std::scoped_lock lock(s_register);
    const auto cCount = s_reactionCount.load();
    if (cCount >= s_reactions.size())
        return 0;
    for (size_t i = 0; i < cCount; ++i)
        if (s_reactions[i].first == aId)
            return 0;
    s_reactions[cCount] = {aId, *apHooks};
    s_reactionCount.store(cCount + 1);
    return 1;
}

std::array<UrGameFormPolicy, 32> s_policies{};
std::array<uint32_t, 32> s_policyOwners{};
std::atomic<size_t> s_policyCount{};
uint32_t RegisterFormPolicy(uint32_t aId, const UrGameFormPolicy* apPolicy)
{
    if (!apPolicy || !aId || (apPolicy->Chat && (!apPolicy->ChatPrefix || !*apPolicy->ChatPrefix || std::strlen(apPolicy->ChatPrefix) > 32)))
        return 0;
    std::scoped_lock lock(s_register);
    size_t count = s_policyCount.load();
    if (count >= s_policies.size() || std::none_of(s_entries.begin(), s_entries.begin() + s_count.load(), [&](const Entry& aEntry) { return aEntry.Id == aId; }))
        return 0;
    for (size_t i = 0; i < count; ++i)
        if (s_policyOwners[i] == aId || (apPolicy->Chat && s_policies[i].Chat && std::strcmp(apPolicy->ChatPrefix, s_policies[i].ChatPrefix) == 0))
            return 0;
    s_policies[count] = *apPolicy;
    s_policyOwners[count] = aId;
    s_policyCount.store(count + 1);
    return 1;
}
template <class F> bool AnyPolicy(F aCheck) noexcept
{
    for (size_t i = 0; i < s_policyCount.load(); ++i)
        if (aCheck(s_policies[i]))
            return true;
    return false;
}
std::array<std::pair<uint32_t, UrGameFollowerPolicy>, 32> s_followerPolicies{};
std::atomic<size_t> s_followerPolicyCount{};
uint32_t RegisterFollowerPolicy(uint32_t aId, const UrGameFollowerPolicy* apPolicy)
{
    if (!aId || !apPolicy || (!apPolicy->Follower && !apPolicy->FollowerWord))
        return 0;
    std::scoped_lock lock(s_register);
    const auto cCount = s_followerPolicyCount.load();
    if (cCount >= s_followerPolicies.size() || std::none_of(s_entries.begin(), s_entries.begin() + s_count.load(), [&](const Entry& aEntry) { return aEntry.Id == aId; }))
        return 0;
    for (size_t i = 0; i < cCount; ++i)
        if (s_followerPolicies[i].first == aId)
            return 0;
    s_followerPolicies[cCount] = {aId, *apPolicy};
    s_followerPolicyCount.store(cCount + 1);
    spdlog::info("GameExtension: add-on {:X} gives follower answers{}{}", aId, apPolicy->Follower ? " (who follows)" : "", apPolicy->FollowerWord ? " (words)" : "");
    return 1;
}
std::array<std::pair<uint32_t, UrGameTouchHooks>, 32> s_touches{};
std::atomic<size_t> s_touchCount{};
uint32_t RegisterTouches(uint32_t aId, const UrGameTouchHooks* apHooks)
{
    if (!aId || !apHooks || !apHooks->Select || !apHooks->Impact)
        return 0;
    std::scoped_lock lock(s_register);
    const auto cCount = s_touchCount.load();
    if (cCount >= s_touches.size())
        return 0;
    for (size_t i = 0; i < cCount; ++i)
        if (s_touches[i].first == aId)
            return 0;
    s_touches[cCount] = {aId, *apHooks};
    s_touchCount.store(cCount + 1);
    return 1;
}
std::array<std::pair<uint32_t, UrGameFallbackPose>, 32> s_fallback{};
std::atomic<size_t> s_fallbackCount{};
uint32_t RegisterFallbackPose(uint32_t aId, const UrGameFallbackPose* apHooks)
{
    if (!aId || !apHooks || !apHooks->Capture || !apHooks->Read || !apHooks->Enabled)
        return 0;
    std::scoped_lock lock(s_register);
    const auto cCount = s_fallbackCount.load();
    if (cCount >= s_fallback.size())
        return 0;
    for (size_t i = 0; i < cCount; ++i)
        if (s_fallback[i].first == aId)
            return 0;
    s_fallback[cCount] = {aId, *apHooks};
    s_fallbackCount.store(cCount + 1);
    return 1;
}
uintptr_t VisualRoot(uint32_t aForm)
{
    auto* pRef = aForm ? Cast<TESObjectREFR>(TESForm::GetById(aForm)) : nullptr;
    return pRef && !pRef->IsDeleted() ? reinterpret_cast<uintptr_t>(pRef->GetNiNode()) : 0;
}
const UrGameApi s_api{
    1,
    sizeof(UrGameApi),
    Register,
    Endpoint,
    PluginForm,
    Papyrus,
    VirtualMachine,
    VisitActors,
    ReadActor,
    WornArmor,
    EquipCopyArmor,
    Reference,
    CreateCopy,
    ApplyReference,
    RemoveCopy,
    SaveGeneration,
    WithSave,
    [](void* apNative, UrGameReference* apOut) { return Describe(static_cast<TESObjectREFR*>(apNative), apOut); },
    RegisterPresentation,
    VRBodySync::CaptureSolvedPose,
    RegisterPose,
    RegisterProjectile,
    Speech,
    StopSpeech,
    Subtitle,
    MoveReference,
    Motion,
    Vehicle,
    GlobalValue,
    ClaimMount,
    [](const char* apText)
    {
        if (apText)
            Utils::ShowHudMessage(apText);
    },
    Interaction,
    RagdollPoseService::Present,
    RegisterRagdoll,
    [](uint32_t aForm) -> uint32_t { return GameExtension::Reserved(aForm); },
    VisitCell,
    RegisterObjects,
    RegisterFormPolicy,
    RegisterReactions,
    [](uint32_t aId, uint32_t aEpoch, uint32_t aKind, float aMagnitude, float aDirection, uint32_t aObserver)
    { return entt::locator<World>::has_value() ? World::Get().ctx().at<HitReactionService>().PresentReaction(aId, aEpoch, aKind, aMagnitude, aDirection, aObserver) : 0; },
    RegisterTouches,
    RegisterFallbackPose,
    VisualRoot,
    []() -> uint32_t { return IsPlayerInMenu(); },
    [](uint32_t aHand, uint32_t aFlags)
    {
#ifdef SKYRIMVR
        VRSwingService::ReportHit(aHand, aFlags);
#else
        (void)aHand;
        (void)aFlags;
#endif
    },
    RegisterFollowerPolicy};
} // namespace

// The game API's layout as add-ons built against version 1 read it: members are only ever appended (UR_GAME_API_HAS).
// A member moved or put in between breaks every add-on already built, so these offsets must never change.
static_assert(sizeof(void*) == 8, "the game API's offsets are pinned for 64-bit builds");
static_assert(offsetof(UrGameApi, Register) == 8);
static_assert(offsetof(UrGameApi, RegisterFormPolicy) == 8 + 35 * 8);
static_assert(offsetof(UrGameApi, PlayerMeleeHit) == 8 + 42 * 8);
static_assert(offsetof(UrGameApi, RegisterFollowerPolicy) == 8 + 43 * 8);
static_assert(sizeof(UrGameApi) == 8 + 44 * 8, "a new member goes at the end; pin it here too");

extern "C" __declspec(dllexport) const UrGameApi* urSovngarde_GetGameApi(uint32_t aVersion)
{
    return aVersion == UR_GAME_API_VERSION ? &s_api : nullptr;
}
void GameExtension::OnCreate(uint32_t aHandle) noexcept
{
    const size_t cCount = s_count.load();
    if (!cCount || !aHandle)
        return;
    UrGameReference ref{};
    if (!Reference(0, aHandle, &ref))
        return;
    for (size_t i = 0; i < cCount; ++i)
        if (s_entries[i].Hooks.Created)
            s_entries[i].Hooks.Created(&ref);
}
bool GameExtension::OnActivate(uint32_t aReference, uint32_t aActivator) noexcept
{
    const size_t cCount = s_count.load();
    for (size_t i = 0; i < cCount; ++i)
        if (s_entries[i].Hooks.Activate && s_entries[i].Hooks.Activate(aReference, aActivator))
            return true;
    return false;
}
void GameExtension::PresentNode(uint32_t aForm, uint32_t aSide, const void* apRoot, const void* apNode, void* apContext, UrCommitPose aCommit) noexcept
{
    const auto cCount = s_presenterCount.load();
    for (size_t i = 0; i < cCount; ++i)
        s_presenters[i].Present(aForm, aSide, apRoot, apNode, apContext, aCommit);
}
void GameExtension::OnProjectile(const UrGameProjectile& acLaunch) noexcept
{
    if (!acLaunch.Handle)
        return;
    const auto cCount = s_projectileCount.load();
    for (size_t i = 0; i < cCount; ++i)
        s_projectiles[i].second(&acLaunch);
}

void GameExtension::OnFrameEnd() noexcept
{
    const size_t cCount = s_count.load();
    for (size_t i = 0; i < cCount; ++i)
        if (s_entries[i].Hooks.FrameEnd)
            s_entries[i].Hooks.FrameEnd();
}

bool GameExtension::ReadPose(uint32_t aServerId, uint32_t aEpoch, uint64_t aTick, glm::vec3& aPosition, float& aHeading, VRPose& aPose) noexcept
{
    const size_t cCount = s_count.load();
    for (size_t i = 0; i < cCount; ++i)
    {
        const auto cSample = s_entries[i].Pose.load();
        UrGamePose frame{};
        if (!cSample || !cSample(aServerId, aEpoch, aTick, &frame))
            continue;
        glm::vec3 origin(frame.Position[0], frame.Position[1], frame.Position[2]);
        if (!(glm::distance(aPosition, origin) <= 1024.f) || !std::isfinite(frame.Heading) || (frame.Scale && (frame.Scale < 50 || frame.Scale > 20000)))
            continue;
        bool valid = true;
        for (const auto* pVector : {frame.Hips, frame.LeftHand, frame.RightHand})
            valid &= glm::dot(glm::vec3(pVector[0], pVector[1], pVector[2]), glm::vec3(pVector[0], pVector[1], pVector[2])) < 512.f * 512.f;
        if (!valid)
            continue;
        aPose.NoBones = false;
        aPose.HasData = aPose.HasLegs = aPose.HasHips = aPose.HasHandCheck = true;
        static_assert(VRPose::kBoneCount == std::size(frame.Bones));
        for (size_t j = 0; j < VRPose::kBoneCount; ++j)
            aPose.Bones[j].Unpack(frame.Bones[j]);
        std::copy_n(frame.Hips, 3, aPose.HipOffset);
        std::copy_n(frame.LeftHand, 3, aPose.LeftHandOffset);
        std::copy_n(frame.RightHand, 3, aPose.RightHandOffset);
        aPose.HasScale = frame.Scale != 0;
        aPose.RootScale = static_cast<uint16_t>(frame.Scale);
        aPosition = origin;
        aHeading = frame.Heading;
        return true;
    }
    if (aPose.HasData && !aPose.NoBones && !aPose.HasLegs)
    {
        uint32_t bones[7]{};
        static_assert(std::size(bones) == VRPose::kBoneCount - VRPose::kUpperBoneCount);
        for (size_t i = 0; i < s_fallbackCount.load(); ++i)
            if (s_fallback[i].second.Read(aServerId, aEpoch, aTick, bones))
            {
                for (size_t b = 0; b < std::size(bones); ++b)
                    aPose.Bones[VRPose::kUpperBoneCount + b].Unpack(bones[b]);
                aPose.HasLegs = true;
                return true;
            }
    }
    return false;
}

bool GameExtension::WantsFallbackPose() noexcept
{
    for (size_t i = 0; i < s_fallbackCount.load(); ++i)
        if (s_fallback[i].second.Enabled())
            return true;
    return false;
}
void GameExtension::CaptureFallbackPose(const uint32_t aBones[7]) noexcept
{
    for (size_t i = 0; i < s_fallbackCount.load(); ++i)
        if (s_fallback[i].second.Enabled())
            s_fallback[i].second.Capture(aBones);
}
bool GameExtension::CapturesRagdoll(uint32_t aForm, const void* apRoot) noexcept
{
    const auto cCount = s_ragdollCount.load();
    for (size_t i = 0; i < cCount; ++i)
        if (s_ragdolls[i].second(aForm, apRoot))
            return true;
    return false;
}
uint32_t GameExtension::TakeObjects(UrGameObjectObservation* apOut, uint32_t aCapacity) noexcept
{
    uint32_t occupied{};
    for (size_t i = 0; i < s_objectCount.load() && occupied < aCapacity; ++i)
        occupied += std::min(s_objects[i].second(apOut + occupied, aCapacity - occupied), aCapacity - occupied);
    return occupied;
}
bool GameExtension::HasReactionObservers() noexcept
{
    return s_reactionCount.load() != 0;
}

void GameExtension::Unpaired(uint32_t aForm, uint32_t aKind, float aMagnitude, float aDirection) noexcept
{
    for (size_t i = 0; i < s_reactionCount.load(); ++i)
        s_reactions[i].second.Unpaired(aForm, aKind, aMagnitude, aDirection);
}
uint32_t GameExtension::GraphReaction(const char* apName) noexcept
{
    for (size_t i = 0; i < s_reactionCount.load(); ++i)
        if (const auto cKind = s_reactions[i].second.Graph(apName))
            return cKind;
    return 0;
}
bool GameExtension::HasTouchObservers() noexcept
{
    return s_touchCount.load() != 0;
}
uint32_t GameExtension::SelectTouch(const UrGameTouch* apPrevious, const UrGameTouch* apCurrent, uint32_t aHasPoint) noexcept
{
    for (size_t i = 0; i < s_touchCount.load(); ++i)
        if (const auto cResult = s_touches[i].second.Select(apPrevious, apCurrent, aHasPoint))
            return cResult;
    return 0;
}
void GameExtension::Impact(uint32_t aForm, uint32_t aSide, float aSpeed, uint64_t aTick, const float aPoint[3]) noexcept
{
    for (size_t i = 0; i < s_touchCount.load(); ++i)
        s_touches[i].second.Impact(aForm, aSide, aSpeed, aTick, aPoint);
}
bool GameExtension::Reserved(uint32_t aForm) noexcept
{
    for (size_t i = 0; i < s_count.load(); ++i)
        if (const auto cReserved = s_entries[i].Reserved.load(); cReserved && cReserved(aForm))
            return true;
    return false;
}
bool GameExtension::BeforeDelete(uint32_t aForm) noexcept
{
    for (size_t i = 0; i < s_count.load(); ++i)
        if (const auto cBefore = s_entries[i].BeforeDelete.load(); cBefore && cBefore(aForm))
            return true;
    return false;
}
void GameExtension::OnGameStep() noexcept
{
    for (size_t i = 0; i < s_count.load(); ++i)
        if (const auto cStep = s_entries[i].Step.load())
            cStep();
}

bool GameExtension::ChecksMagic() noexcept
{
    return AnyPolicy([](const auto& acPolicy) { return acPolicy.RejectMagic != nullptr; });
}
bool GameExtension::RejectMagic(const GameId& acForm) noexcept
{
    return AnyPolicy([&](const auto& acPolicy) { return acPolicy.RejectMagic && acPolicy.RejectMagic(acForm.ModId, acForm.BaseId); });
}
bool GameExtension::LocalMagic(uint32_t aForm) noexcept
{
    return AnyPolicy([&](const auto& acPolicy) { return acPolicy.LocalMagic && acPolicy.LocalMagic(aForm); });
}
bool GameExtension::AllowsEffect(uint32_t aArmor, uint32_t aSlot, uint32_t aSpell, uint32_t aEffect) noexcept
{
    return AnyPolicy([&](const auto& acPolicy) { return acPolicy.AllowSelfEffect && acPolicy.AllowSelfEffect(aArmor, aSlot, aSpell, aEffect); });
}
uint32_t GameExtension::FollowerAnswer(uint32_t aActorForm, uint32_t aBaseForm, bool aTeammate) noexcept
{
    const size_t cCount = s_followerPolicyCount.load();
    // An add-on's callback that asks core about actors (Actor, VisitActors) comes back here: core's rule answers that inner question.
    thread_local bool s_asking = false;
    if (cCount == 0 || s_asking)
        return FollowerPolicy::kDefer;
    s_asking = true;
    std::array<uint32_t, 32> answers{};
    for (size_t i = 0; i < cCount; ++i)
        if (const auto* pAsk = s_followerPolicies[i].second.Follower)
            answers[i] = pAsk(aActorForm, aBaseForm, aTeammate ? 1u : 0u);
    s_asking = false;
    return FollowerPolicy::Merge(answers.data(), cCount);
}

uint8_t GameExtension::FollowerWord(uint32_t aActorForm, uint32_t aCommand) noexcept
{
    for (size_t i = 0; i < s_followerPolicyCount.load(); ++i)
        if (const auto* pWord = s_followerPolicies[i].second.FollowerWord)
            if (const uint8_t cReply = FollowerPolicy::WordReply(pWord(aActorForm, aCommand)))
                return cReply;
    return 0;
}

bool GameExtension::SharedSwitch(uint32_t aReference, uint32_t aBase, uint32_t aType) noexcept
{
    return AnyPolicy([&](const auto& acPolicy) { return acPolicy.SharedSwitch && acPolicy.SharedSwitch(aReference, aBase, aType); });
}
void GameExtension::Module(uint32_t aId, const char* apFilename)
{
    for (size_t i = 0; i < s_policyCount.load(); ++i)
        if (auto callback = s_policies[i].Module)
            callback(aId, apFilename);
}
bool GameExtension::Chat(std::string_view acMessage)
{
    const auto cSpace = acMessage.find(' ');
    return AnyPolicy(
        [&](const auto& acPolicy)
        {
            if (!acPolicy.Chat || acMessage.substr(0, cSpace) != acPolicy.ChatPrefix)
                return false;
            if (!entt::locator<World>::has_value())
                return false;
            auto body = std::string(cSpace == acMessage.npos ? std::string_view{} : acMessage.substr(cSpace + 1));
            World::Get().GetRunner().Queue([fn = acPolicy.Chat, text = std::move(body)] { fn(text.data(), static_cast<uint32_t>(text.size())); });
            return true;
        });
}
