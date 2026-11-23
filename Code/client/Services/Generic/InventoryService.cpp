#include <Services/GameExtension.h>
#include <Services/InventoryService.h>
#include <Helpers/Throttle.h>
#include <Services/Generic/NpcEquipmentCheck.h>
#include <Services/Generic/CopyLootCheck.h>
#include <Services/Generic/PlayerCopyWeapon.h>
#include <Services/Generic/AlchemyMadeRules.h>
#include <Structs/ItemDataChange.h>
#include <Copies/EpochMiss.h>
#include <Helpers/PerfScope.h>
#include <Copies/RecentModelChanges.h>
#include <FollowerCommand.h>
#include <FollowerInventory.h>

#include <Messages/RequestObjectInventoryChanges.h>
#include <Messages/NotifyObjectInventoryChanges.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/RequestEquipmentChanges.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/DrawWeaponRequest.h>
#include <Messages/NotifyDrawWeapon.h>
#include <Messages/CharacterSpawnRequest.h>

#include <Events/UpdateEvent.h>
#include <Events/InventoryChangeEvent.h>
#include <Events/ItemTakeEvent.h>
#include <Events/EquipmentChangeEvent.h>

#include <World.h>
#include <Games/Skyrim/Interface/UI.h>
#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Actor.h>
#include <Components/BGSEquipType.h>
#include <Structs/ObjectData.h>
#include <Forms/TESWorldSpace.h>
#include <Games/TES.h>
#include <Games/Overrides.h>
#include <EquipManager.h>
#include <Games/ActorExtension.h>
#include <Forms/TESNPC.h>
#include <DefaultObjectManager.h>
#include <PlayerCharacter.h>
#include <Forms/MagicItem.h>
#include <Forms/AlchemyItem.h>
#include <Games/Skyrim/GameSound.h>
#include <Forms/TESObjectARMO.h>
#include <ItemExtraData.h>

namespace
{
// Defined with ApplyHandEquipment below: the record of what this side put in a remote player copy's hands.
void NoteHandItem(Actor* apActor, TESForm* apItem, bool aLeft, bool aEquipped) noexcept;
} // namespace

InventoryService::InventoryService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&InventoryService::OnUpdate>(this);
    m_inventoryConnection = m_dispatcher.sink<InventoryChangeEvent>().connect<&InventoryService::OnInventoryChangeEvent>(this);
    m_equipmentConnection = m_dispatcher.sink<EquipmentChangeEvent>().connect<&InventoryService::OnEquipmentChangeEvent>(this);
    m_inventoryChangeConnection = m_dispatcher.sink<NotifyInventoryChanges>().connect<&InventoryService::OnNotifyInventoryChanges>(this);
    m_equipmentChangeConnection = m_dispatcher.sink<NotifyEquipmentChanges>().connect<&InventoryService::OnNotifyEquipmentChanges>(this);
    m_drawWeaponConnection = m_dispatcher.sink<NotifyDrawWeapon>().connect<&PlayerCopyWeapon::OnNotify>();
    m_spawnConnection = m_dispatcher.sink<CharacterSpawnRequest>().connect<&PlayerCopyWeapon::OnSpawn>();
}

void InventoryService::OnUpdate(const UpdateEvent& acUpdateEvent) noexcept
{
    PerfScope perfScope("InventoryService::OnUpdate");

    RunWeaponStateUpdates();
    RunNakedNPCBugChecks();
    RunEquipmentSnapshotUpdates();
    NpcEquipmentCheck::Run(m_world, m_transport);
    CopyLootCheck::Run();
    PlayerCopyWeapon::Run(m_world, m_transport.IsConnected());
}

void InventoryService::OnInventoryChangeEvent(const InventoryChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    // An unsupported local plugin has no network item identity.
    if (!acEvent.Item.BaseId)
        return;

    auto view = m_world.view<FormIdComponent>();

    const auto iter = Utils::FindById<FormIdComponent>(view, acEvent.FormId);

    if (!iter)
        return;

    uint32_t serverId = 0;
    if (acEvent.OwnershipEpoch != 0)
    {
        const auto* pLocalComponent = m_world.try_get<LocalComponent>(*iter);
        const auto* pRemoteComponent = m_world.try_get<RemoteComponent>(*iter);
        const bool ownershipMatches = (pLocalComponent && pLocalComponent->Id == acEvent.ServerId && pLocalComponent->OwnershipEpoch == acEvent.OwnershipEpoch)
            || (pRemoteComponent && pRemoteComponent->Id == acEvent.ServerId && pRemoteComponent->OwnershipEpoch == acEvent.OwnershipEpoch);

        if (!ownershipMatches)
        {
            spdlog::debug("Discarded an inventory change for actor {:X} because ownership changed after it was queued (epoch {})", acEvent.ServerId, acEvent.OwnershipEpoch);
            return;
        }

        serverId = acEvent.ServerId;
    }
    else
    {
        if (Cast<Actor>(TESForm::GetById(acEvent.FormId)))
            return;

        const std::optional<uint32_t> serverIdRes = Utils::GetServerId(*iter);
        if (!serverIdRes)
        {
            spdlog::warn(
                "Discarded inventory change for form {:X} because it has no server entity (item {:X}, count {})", acEvent.FormId, acEvent.Item.BaseId.BaseId, acEvent.Item.Count);
            return;
        }
        serverId = *serverIdRes;
    }

    RequestInventoryChanges request;
    request.ServerId = serverId;
    request.OwnershipEpoch = acEvent.OwnershipEpoch;
    request.Item = acEvent.Item;
    request.Drop = acEvent.Drop;
    request.UpdateClients = acEvent.UpdateClients;
    request.PickedUp = acEvent.PickedUp;

    m_transport.Send(request);

    // Another player may take the same item from it at once: the server's answer could take it back off the player (ItemTakeService).
    if (acEvent.IntoPlayer && acEvent.Item.Count < 0)
    {
        ItemTakeEvent take{ItemTakeSource::kInventory, serverId, {}, {}, acEvent.Item};
        take.Item.Count = -acEvent.Item.Count;
        m_dispatcher.trigger(take);
    }

    spdlog::info("Sending item request, item: {:X}, count: {}, target object: {:X}, drop: {}", acEvent.Item.BaseId.BaseId, acEvent.Item.Count, acEvent.FormId, acEvent.Drop);
}

void InventoryService::OnEquipmentChangeEvent(const EquipmentChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);

    const auto iter = Utils::FindById<FormIdComponent>(view, acEvent.ActorId);

    if (!iter)
        return;

    const auto* pLocalComponent = m_world.try_get<LocalComponent>(*iter);
    if (acEvent.OwnershipEpoch == 0 || !pLocalComponent || pLocalComponent->Id != acEvent.ServerId || pLocalComponent->OwnershipEpoch != acEvent.OwnershipEpoch)
    {
        spdlog::debug("Discarded an equipment change for actor {:X} because ownership changed after it was queued (epoch {})", acEvent.ServerId, acEvent.OwnershipEpoch);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.ActorId));
    if (!pActor)
        return;

    auto& modSystem = World::Get().GetModSystem();

    RequestEquipmentChanges request;
    request.ServerId = acEvent.ServerId;
    request.OwnershipEpoch = acEvent.OwnershipEpoch;

    if (!modSystem.GetServerModId(acEvent.EquipSlotId, request.EquipSlotId))
        return;
    if (acEvent.IsSpell && GameExtension::ChecksMagic())
        modSystem.GetSharedSpellId(acEvent.ItemId, request.ItemId);
    else if (!modSystem.GetServerModId(acEvent.ItemId, request.ItemId))
        return;

    request.Count = acEvent.Count;
    request.Unequip = acEvent.Unequip;
    request.IsSpell = acEvent.IsSpell;
    request.IsShout = acEvent.IsShout;
    request.IsAmmo = acEvent.IsAmmo;
    request.CurrentInventory = pActor->GetEquipment();

    m_transport.Send(request);

    spdlog::info("Sending equipment request, item: {:X}, count: {}, target object: {:X}{}", acEvent.ItemId, acEvent.Count, acEvent.ActorId, acEvent.Unequip ? " (unequip)" : "");
    // The hook queues the change before the game makes it, and the equipment above is read a frame later.
    // An item taken off but worn by then, or put on but not worn, was undone through a path no hook sees: the first check sends what it wears.
    // An item eaten or drunk is never worn: nothing to check afterwards.
    const TESForm* pEventItem = TESForm::GetById(acEvent.ItemId);
    if (pActor->formID != 0x14 && !(pEventItem && pEventItem->IsConsumable()))
    {
        const bool cUndone = !acEvent.IsSpell && !acEvent.IsShout && NpcEquipmentCheck::IsWorn(request.CurrentInventory, request.ItemId) == acEvent.Unequip;
        NpcEquipmentCheck::Note(pActor->formID, request.ServerId, request.OwnershipEpoch, request.CurrentInventory, cUndone);
    }
}

namespace
{
// Skyrim.esm sound descriptors. An ingredient has no eating sound of its own (no field, no default object): the food one
// is the game's only eating sound. The making ones are what the alchemy menu plays for its own player.
constexpr uint32_t kFoodEatSound = 0xCAF94;    // ITMFoodEat
constexpr uint32_t kPotionMadeSound = 0xC8C74; // UIAlchemyCreatePotion
constexpr uint32_t kPoisonMadeSound = 0xC8C6F; // UIAlchemyCreatePoison

// Other players' makings at an alchemy table, from what their inventories do (AlchemyMadeRules).
AlchemyMadeRules::Tracker s_alchemyMade;

/**
 * Play the sound of a potion, poison, food item or ingredient being used, at a copy whose owner ate or drank it.
 * An item whose sound is not set plays none.
 *
 * @param apActor The copy.
 * @param apItem The item its owner used.
 */
void PlayConsumeSound(Actor* apActor, TESForm* apItem) noexcept
{
    const auto* pAlchemy = apItem->formType == FormType::Alchemy ? Cast<AlchemyItem>(apItem) : nullptr;
    const auto* pSoundForm = pAlchemy ? static_cast<const TESForm*>(pAlchemy->data.pConsumptionSound) : nullptr;
    uint32_t sound = pSoundForm ? pSoundForm->formID : 0;
    if (apItem->formType == FormType::Ingredient)
    {
        sound = kFoodEatSound;
        s_alchemyMade.NoteEaten(apActor->formID, apItem->formID, std::chrono::steady_clock::now());
    }
    const bool cPlayed = sound && GameSound::Play(sound, &apActor->position);
    spdlog::debug("Equipment sync: remote actor {:X} used {:X}; its sound {:X} {}", apActor->formID, apItem->formID, sound, cPlayed ? "played" : "not played");
}

/**
 * Another player's inventory change, as it reaches their copy: an ingredient used up and a made potion or poison gained
 * close together is a making at an alchemy table, whose sound plays at the copy (one per making).
 *
 * @param apActor The other player's copy.
 * @param acItem The change (count below zero: removed).
 */
void NoteAlchemyMade(Actor* apActor, const Inventory::Entry& acItem) noexcept
{
    using AlchemyMadeRules::Made;
    const auto cNow = std::chrono::steady_clock::now();
    Made made = Made::None;
    if (acItem.Count > 0 && acItem.BaseId.ModId == ItemDataChanges::kRunTimeMod && acItem.Data.Base.IsSet())
        made = s_alchemyMade.NoteMade(apActor->formID, acItem.Data.Base.IsPoison, cNow);
    else if (acItem.Count < 0 && acItem.BaseId.ModId != ItemDataChanges::kRunTimeMod)
    {
        const TESForm* pItem = TESForm::GetById(World::Get().GetModSystem().GetGameId(acItem.BaseId));
        if (pItem && pItem->formType == FormType::Ingredient)
            made = s_alchemyMade.NoteIngredientUsed(apActor->formID, pItem->formID, cNow);
    }
    if (made == Made::None)
        return;

    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    const bool cHeard = pPlayer && AlchemyMadeRules::Heard(glm::distance(glm::vec3(apActor->position), glm::vec3(pPlayer->position)));
    const uint32_t cSound = made == Made::Poison ? kPoisonMadeSound : kPotionMadeSound;
    const bool cPlayed = cHeard && GameSound::Play(cSound, &apActor->position);
    spdlog::debug(
        "Equipment sync: remote player {:X} made a {} at an alchemy table; its sound {}", apActor->formID, made == Made::Poison ? "poison" : "potion",
        cPlayed ? "played" : (cHeard ? "not played" : "too far away to be heard"));
}

// True for this player's own follower, whose gear is what this game has.
// Another player's game dresses her in its own save's outfit and sends that as a re-equip; it is ignored here.
bool IsMyFollower(const Actor* apActor, const uint32_t aServerId, const char* apWhat) noexcept
{
    if (!apActor || !apActor->IsPlayerFollower())
        return false;
    static std::chrono::steady_clock::time_point s_nextNote{};
    const auto now = std::chrono::steady_clock::now();
    if (now >= s_nextNote)
    {
        s_nextNote = now + std::chrono::seconds(10);
        spdlog::info("Follower {:X} (server id {:X}): {} from another player not applied; she keeps her gear here", apActor->formID, aServerId, apWhat);
    }
    return true;
}
} // namespace

void InventoryService::OnNotifyInventoryChanges(const NotifyInventoryChanges& acMessage) noexcept
{
    if (acMessage.OwnershipEpoch != 0)
    {
        Actor* pActor = nullptr;
        bool currentLocalOwner = false;

        auto remoteView = m_world.view<RemoteComponent, FormIdComponent>(entt::exclude<LocalComponent>);
        const auto remoteIt = std::find_if(remoteView.begin(), remoteView.end(), [remoteView, &acMessage](const entt::entity aEntity)
        {
            const auto& remoteComponent = remoteView.get<RemoteComponent>(aEntity);
            return remoteComponent.Id == acMessage.ServerId && remoteComponent.OwnershipEpoch == acMessage.OwnershipEpoch;
        });

        if (remoteIt != remoteView.end())
            pActor = Cast<Actor>(TESForm::GetById(remoteView.get<FormIdComponent>(*remoteIt).Id));
        else
        {
            auto localView = m_world.view<LocalComponent, FormIdComponent>();
            const auto localIt = std::find_if(localView.begin(), localView.end(), [localView, &acMessage](const entt::entity aEntity)
            {
                const auto& localComponent = localView.get<LocalComponent>(aEntity);
                return localComponent.Id == acMessage.ServerId && localComponent.OwnershipEpoch == acMessage.OwnershipEpoch;
            });

            if (localIt != localView.end())
            {
                pActor = Cast<Actor>(TESForm::GetById(localView.get<FormIdComponent>(*localIt).Id));
                currentLocalOwner = true;
            }
        }

        // A character here under another epoch is a missed update, not an unknown character: report it.
        if (!pActor)
        {
            const auto byIdIt = Utils::FindById<RemoteComponent>(remoteView, acMessage.ServerId);
            if (byIdIt)
                ReportEpochMiss("an equipment change", acMessage.ServerId, acMessage.OwnershipEpoch, remoteView.get<RemoteComponent>(*byIdIt).OwnershipEpoch);
        }

        if (!pActor)
        {
            spdlog::debug("Discarded an inventory update for actor {:X} because epoch {} is no longer current", acMessage.ServerId, acMessage.OwnershipEpoch);
            return;
        }
        const auto* pExtension = pActor->GetExtension();
        const bool cOwnerFollower = FollowerCommand::MayApplyOwnerInventory(
            {currentLocalOwner, pExtension && pExtension->IsLocal(), !pExtension || pExtension->IsPlayer(), pExtension && pExtension->IsOthersFollower(),
             pActor->IsPlayerFollower()});
        if (!cOwnerFollower && IsMyFollower(pActor, acMessage.ServerId, "an inventory change"))
            return;

        ScopedInventoryOverride _;
        const Inventory::Entry item = cOwnerFollower ? FollowerOwnerItem(acMessage.Item) : acMessage.Item;

        if (acMessage.Drop)
            pActor->DropOrPickUpObject(item, nullptr, nullptr);
        else
            pActor->AddOrRemoveItem(item, false, acMessage.PickedUp);
        CopyLootCheck::OnOwnerChange(pActor->formID, item, acMessage.Drop);
        if (!currentLocalOwner && pExtension && pExtension->IsPlayer() && !acMessage.Drop && !acMessage.PickedUp)
            NoteAlchemyMade(pActor, item);

        if (cOwnerFollower)
            spdlog::debug(
                "FollowerInventory: {:X}:{:X} x{} applied to follower {:X} (server id {:X}, epoch {}); worn flags from other games cleared", item.BaseId.ModId, item.BaseId.BaseId,
                item.Count, pActor->formID, acMessage.ServerId, acMessage.OwnershipEpoch);

        return;
    }

    TESObjectREFR* pObject = Utils::GetByServerId<TESObjectREFR>(acMessage.ServerId);
    if (!pObject)
        return;

    ScopedInventoryOverride _;
    pObject->AddOrRemoveItem(acMessage.Item);
}

void InventoryService::OnNotifyEquipmentChanges(const NotifyEquipmentChanges& acMessage) noexcept
{
    auto view = m_world.view<RemoteComponent, FormIdComponent>(entt::exclude<LocalComponent>);
    const auto it = std::find_if(view.begin(), view.end(), [view, &acMessage](const entt::entity aEntity)
    {
        const auto& remoteComponent = view.get<RemoteComponent>(aEntity);
        return remoteComponent.Id == acMessage.ServerId && remoteComponent.OwnershipEpoch == acMessage.OwnershipEpoch;
    });
    if (it == view.end())
    {
        spdlog::debug("Discarded an equipment update for actor {:X} because epoch {} is no longer current", acMessage.ServerId, acMessage.OwnershipEpoch);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(*it).Id));
    if (!pActor)
        return;
    if (IsMyFollower(pActor, acMessage.ServerId, "an equipment change"))
        return;

    // What the owner wears now is what a later loot check sets the copy back to, applied here or not.
    CopyLootCheck::OnOwnerEquipment(pActor->formID, acMessage.CurrentInventory);

    // The game's equip code reads the actor's AI process, and an actor not loaded here has none.
    // What it wears comes with its next assignment.
    if (!pActor->HasLoadedProcess())
    {
        spdlog::info("Equipment sync: remote actor {:X} has no AI process here; change not applied", pActor->formID);
        return;
    }

    // No item: an equipment snapshot (see RunEquipmentSnapshotUpdates).
    if (!acMessage.ItemId)
    {
        if (pActor->GetExtension()->IsRemote())
        {
            ApplyHandEquipment(pActor, acMessage.CurrentInventory);
            NpcEquipmentCheck::ApplyWornItems(pActor, acMessage.CurrentInventory);
        }
        return;
    }

    // The owner's worn list comes with every change: a piece on it now that a later snapshot lacks was taken off (NpcEquipmentCheck::ApplyWornItems).
    if (!acMessage.IsSpell && !acMessage.IsShout)
        NpcEquipmentCheck::NoteOwnerEquipment(pActor, acMessage.CurrentInventory, true);

    auto& modSystem = World::Get().GetModSystem();

    spdlog::info("Equipment sync: remote actor {:X} {} {:X}:{:X} (spell: {}, shout: {}, slot {:X}:{:X})", pActor->formID, acMessage.Unequip ? "unequips" : "equips", acMessage.ItemId.ModId, acMessage.ItemId.BaseId, acMessage.IsSpell, acMessage.IsShout, acMessage.EquipSlotId.ModId, acMessage.EquipSlotId.BaseId);

    uint32_t itemId = acMessage.IsSpell ? modSystem.GetLocalSpellId(acMessage.ItemId) : modSystem.GetGameId(acMessage.ItemId);
    TESForm* pItem = TESForm::GetById(itemId);

    if (!pItem)
    {
        spdlog::error("Could not find inventory item {:X}:{:X}", acMessage.ItemId.ModId, acMessage.ItemId.BaseId);
        return;
    }

    // Eaten or drunk by its owner: only its sound is played here. Equipping it would use up the copy's own and apply its effect again.
    if (pItem->IsConsumable())
    {
        if (!acMessage.Unequip)
            PlayConsumeSound(pActor, pItem);
        return;
    }

    // The equip makes other plugins read the copy's hands.
    pActor->EmptyHandsOfStrayExtraData("its item does not hold, before an equip from its owner");

    uint32_t equipSlotId = modSystem.GetGameId(acMessage.EquipSlotId);
    TESForm* pEquipSlot = TESForm::GetById(equipSlotId);

    uint32_t slotId = 0;
    if (pEquipSlot == DefaultObjectManager::Get().rightEquipSlot)
        slotId = 1;

    auto* pEquipManager = EquipManager::Get();

    if (acMessage.IsSpell)
    {
        if (acMessage.Unequip)
            pEquipManager->UnEquipSpell(pActor, pItem, slotId);
        else
            pEquipManager->EquipSpell(pActor, pItem, slotId);

        return;
    }
    else if (acMessage.IsShout)
    {
        if (acMessage.Unequip)
            pEquipManager->UnEquipShout(pActor, pItem);
        else
            pEquipManager->EquipShout(pActor, pItem);

        return;
    }

    // TODO: ExtraData necessary? probably
    if (acMessage.Unequip)
    {
        pEquipManager->UnEquip(pActor, pItem, nullptr, acMessage.Count, pEquipSlot, false, true, false, false, nullptr);
        NoteHandItem(pActor, pItem, pEquipSlot == DefaultObjectManager::Get().leftEquipSlot, false);
    }
    else
    {
        // A weapon already in that hand is not equipped again.
        // Re-equipping it empties the copy's hand while the game still reports it equipped (`live-equip-twice`).
        if (pItem->formType != FormType::Armor)
        {
            const uint32_t hand = pEquipSlot == DefaultObjectManager::Get().leftEquipSlot ? 0 : 1;
            if (pActor->GetEquippedWeapon(hand) == pItem)
            {
                spdlog::info("Equipment sync: remote actor {:X} already holds {:X} in the {} hand; equip skipped", pActor->formID, pItem->formID, hand == 0 ? "left" : "right");
                return;
            }
        }

        // Unequip all armor first, since the game won't auto unequip armor
        Inventory wornArmor{};
        if (pItem->formType == FormType::Armor)
        {
            wornArmor = pActor->GetWornArmor();
            for (const auto& armor : wornArmor.Entries)
            {
                uint32_t armorId = modSystem.GetGameId(armor.BaseId);
                TESForm* pArmor = TESForm::GetById(armorId);
                if (pArmor)
                    pEquipManager->UnEquip(pActor, pArmor, nullptr, 1, pEquipSlot, false, true, false, false, nullptr);
            }
        }

        // The stack its owner wears: an enchanted, tempered, poisoned or named one is not a plain one of the same item.
        const bool cLeft = pEquipSlot == DefaultObjectManager::Get().leftEquipSlot;
        const auto cWorn = std::find_if(
            acMessage.CurrentInventory.Entries.begin(), acMessage.CurrentInventory.Entries.end(),
            [&acMessage, cLeft](const Inventory::Entry& acEntry) { return acEntry.BaseId == acMessage.ItemId && (cLeft ? acEntry.ExtraWornLeft : acEntry.ExtraWorn); });
        ExtraDataList* pStack = cWorn != acMessage.CurrentInventory.Entries.end() ? ItemExtraData::FindStack(pActor, pItem, *cWorn) : nullptr;

        pEquipManager->Equip(pActor, pItem, pStack, acMessage.Count, pEquipSlot, false, true, false, false);
        NoteHandItem(pActor, pItem, cLeft, true);

        // The owner's game took off what the new armor replaces without a change of its own, so that piece stays off here.
        // Put back on, it would take its slot back: a shield or helmet changed for another would show the old one.
        // Only when the copy has the new piece: an equip of one it lacks does nothing, and the slot would stay empty.
        const auto* pNewArmor = pActor->GetItemCountInInventory(pItem) > 0 ? Cast<TESObjectARMO>(pItem) : nullptr;
        for (const auto& armor : wornArmor.Entries)
        {
            uint32_t armorId = modSystem.GetGameId(armor.BaseId);
            TESForm* pArmor = TESForm::GetById(armorId);
            const auto* pOldArmor = Cast<TESObjectARMO>(pArmor);
            if (pNewArmor && pOldArmor && pOldArmor != pNewArmor && (pOldArmor->slotType & pNewArmor->slotType) != 0)
                continue;
            if (pArmor)
                pEquipManager->Equip(pActor, pArmor, nullptr, 1, pEquipSlot, false, true, false, false);
        }
    }
}

void InventoryService::RunWeaponStateUpdates() noexcept
{
    if (!m_transport.IsConnected())
        return;

    // This player's own draw state is read every frame and sent the frame it changes: a VR draw from a holster or a grip
    // reaches the other games' copies only this way, and the half-second poll made it up to about 2 s late there.
    // NPCs stay on the poll; their draw travels with their animations.
    static Throttle s_sendThrottle(500ms);
    const bool cNpcTurn = s_sendThrottle.TakeTurn();

    auto view = m_world.view<FormIdComponent, LocalComponent>();

    for (auto entity : view)
    {
        const auto& formIdComponent = view.get<FormIdComponent>(entity);
        if (!cNpcTurn && formIdComponent.Id != 0x14)
            continue;
        Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        // Gone from the game, for instance in a load, before discovery has noticed.
        if (!pActor)
            continue;
        auto& localComponent = view.get<LocalComponent>(entity);

        bool isWeaponDrawn = pActor->actorState.IsWeaponDrawn();
        if (isWeaponDrawn != localComponent.IsWeaponDrawn)
        {
            localComponent.IsWeaponDrawn = isWeaponDrawn;

            DrawWeaponRequest request;
            request.Id = localComponent.Id;
            request.IsWeaponDrawn = isWeaponDrawn;

            m_transport.Send(request);
        }
    }
}

void InventoryService::RunNakedNPCBugChecks() noexcept
{
    if (!m_transport.IsConnected())
        return;

    // How many times this has tried to dress each actor. See the give-up below.
    static TiltedPhoques::Map<uint32_t, uint32_t> s_resets;

    static Throttle s_sendThrottle(1000ms);
    if (!s_sendThrottle.TakeTurn())
        return;

    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);

    for (auto entity : view)
    {
        const auto& formIdComponent = view.get<FormIdComponent>(entity);
        Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor)
            continue;

        const ActorExtension* pExtension = pActor->GetExtension();
        if (!pExtension || pExtension->IsPlayer())
            continue;

        if (pActor->IsDead())
            continue;

        if (pActor->IsWearingBodyPiece())
        {
            s_resets.erase(pActor->formID);
            continue;
        }

        if (!pActor->ShouldWearBodyPiece())
            continue;

        // Give up on an actor this cannot dress.
        // Each ResetInventory emits equip and unequip events that the other players replay as actions.
        // If IsWearingBodyPiece never turns true (a mod-managed outfit, say), a reset every second would flood their queue.
        // After three tries the actor is left dressed as it is.
        constexpr uint32_t cMaxResets = 3;
        auto& resets = s_resets[pActor->formID];
        if (resets >= cMaxResets)
            continue;

        // Don't broadcast changes, it'll just make things messier.
        // If all clients have this problem, they'll all fix it individually.
        ScopedEquipOverride seo;
        ScopedInventoryOverride sio;

        RecentModelChanges::Note(pActor, RecentModelChanges::Kind::ItemsChanged);
        pActor->ResetInventory(false);

        if (++resets == cMaxResets)
        {
            const auto* pBase = Cast<TESNPC>(pActor->baseForm);
            spdlog::warn("Naked NPC check gave up on actor {:X} ({}) after {} resets; it never reported a body piece, and retrying floods the other players with equip actions", pActor->formID,
                         pBase && pBase->fullName.value.AsAscii() ? pBase->fullName.value.AsAscii() : "?", cMaxResets);
        }
    }

    // Forget every count once the map grows, so a cell revisit starts fresh.
    if (s_resets.size() > 256)
        s_resets.clear();
}

void InventoryService::RunEquipmentSnapshotUpdates() noexcept
{
    if (!m_transport.IsConnected())
        return;

    static Throttle s_sendThrottle(1000ms);
    if (!s_sendThrottle.TakeTurn())
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    auto view = m_world.view<FormIdComponent, LocalComponent>();
    const auto it = Utils::FindById<FormIdComponent>(view, 0x14);
    if (!it)
        return;

    const uint32_t serverId = view.get<LocalComponent>(*it).Id;
    const uint32_t ownershipEpoch = view.get<LocalComponent>(*it).OwnershipEpoch;

    Inventory equipment;
    {
        // Measured: reads the whole inventory to keep the worn entries. The perf line reports it per 30 s.
        PerfCounterScope perfScope(PerfCounter::kEquipmentSnapshot);
        equipment = pPlayer->GetEquipment();
    }

    // Compare only what is worn where: charges and similar extra data change constantly in combat.
    if (serverId == m_lastSnapshotServerId && NpcEquipmentCheck::HasSameWornItems(equipment, m_lastEquipmentSnapshot))
        return;

    RequestEquipmentChanges request;
    request.ServerId = serverId;
    // Required: the server silently drops an equipment change whose epoch does not match the owner's.
    // This snapshot reconciles changes missed while a player was out of range, where equipment does not travel.
    request.OwnershipEpoch = ownershipEpoch;
    request.CurrentInventory = equipment;

    m_transport.Send(request);

    m_lastSnapshotServerId = serverId;
    m_lastEquipmentSnapshot = std::move(equipment);
}

namespace
{
struct HandItem
{
    TESForm* pForm;
    bool Left;
};

bool ContainsHandItem(const Vector<HandItem>& acItems, TESForm* apForm, bool aLeft) noexcept
{
    return std::any_of(acItems.begin(), acItems.end(), [apForm, aLeft](const HandItem& aItem) { return aItem.pForm == apForm && aItem.Left == aLeft; });
}

// What this side has put in each remote player copy's hands since it appeared, by form id.
// The worn flags cannot stand in: SetInventory equips before the copy has its 3D, so they say "held" while the hands stay empty.
TiltedPhoques::Map<uint32_t, Vector<HandItem>> s_handsApplied;

void NoteHandItem(Actor* apActor, TESForm* apItem, bool aLeft, bool aEquipped) noexcept
{
    constexpr uint8_t cLightFormType = 31;
    if (!apActor || !apItem || (apItem->formType != FormType::Weapon && static_cast<uint8_t>(apItem->formType) != cLightFormType))
        return;
    Vector<HandItem>& held = s_handsApplied[apActor->formID];
    held.erase(std::remove_if(held.begin(), held.end(), [apItem, aLeft](const HandItem& aItem) { return aItem.pForm == apItem && aItem.Left == aLeft; }), held.end());
    if (aEquipped)
        held.push_back({apItem, aLeft});
}
} // namespace

void InventoryService::ApplyHandEquipment(Actor* apActor, const Inventory& acEquipment, bool aArrival) noexcept
{
    // The equips below make other plugins read the actor's hands.
    apActor->EmptyHandsOfStrayExtraData("its item does not hold, before its owner's hand items");

    constexpr uint8_t cLightFormType = 31;

    auto& modSystem = World::Get().GetModSystem();
    auto* pEquipManager = EquipManager::Get();
    auto& defaultObjects = DefaultObjectManager::Get();

    const auto collectHandItems = [&modSystem](const Inventory& acInventory)
    {
        Vector<HandItem> items;
        for (const auto& entry : acInventory.Entries)
        {
            if (!entry.IsWorn())
                continue;

            TESForm* pForm = TESForm::GetById(modSystem.GetGameId(entry.BaseId));
            if (!pForm || (pForm->formType != FormType::Weapon && static_cast<uint8_t>(pForm->formType) != cLightFormType))
                continue;

            if (entry.ExtraWorn)
                items.push_back({pForm, false});
            if (entry.ExtraWornLeft)
                items.push_back({pForm, true});
        }
        return items;
    };

    const auto containsItem = [](const Vector<HandItem>& acItems, const HandItem& acItem)
    { return std::any_of(acItems.begin(), acItems.end(), [&acItem](const HandItem& aItem) { return aItem.pForm == acItem.pForm && aItem.Left == acItem.Left; }); };

    const Vector<HandItem> desiredItems = collectHandItems(acEquipment);
    Vector<HandItem>& heldItems = s_handsApplied[apActor->formID];
    if (aArrival)
        heldItems.clear(); // a fresh copy: SetInventory set the worn flags, the hands are empty
    // A hand the hand check emptied no longer holds what we put there: it is put there again below while its owner still holds it.
    for (const auto& [pForm, cLeft] : TESObjectREFR::TakeEmptiedHands(apActor->formID))
        NoteHandItem(apActor, pForm, cLeft, false);
    const Vector<HandItem> flaggedItems = collectHandItems(apActor->GetEquipment());
    spdlog::info("Equipment sync: remote actor {:X} hands {}: {} hand items wanted ({} worn entries in all), {} put there by us, {} flagged worn, left spell {}, right spell {}", apActor->formID,
                 aArrival ? "on arrival" : "on snapshot", desiredItems.size(), acEquipment.Entries.size(), heldItems.size(), flaggedItems.size(),
                 acEquipment.CurrentMagicEquipment.LeftHandSpell ? "yes" : "no", acEquipment.CurrentMagicEquipment.RightHandSpell ? "yes" : "no");

    // Off: what we put there and what the container flags as worn, when the owner no longer holds it.
    Vector<HandItem> toUnequip = heldItems;
    for (const auto& item : flaggedItems)
        if (!ContainsHandItem(toUnequip, item.pForm, item.Left))
            toUnequip.push_back(item);
    for (const auto& item : toUnequip)
    {
        if (containsItem(desiredItems, item))
            continue;
        spdlog::info("Equipment sync: remote actor {:X} unequips {:X} ({} hand)", apActor->formID, item.pForm->formID, item.Left ? "left" : "right");
        pEquipManager->UnEquip(apActor, item.pForm, nullptr, 1, item.Left ? defaultObjects.leftEquipSlot : defaultObjects.rightEquipSlot, false, true, false, false, nullptr);
        NoteHandItem(apActor, item.pForm, item.Left, false);
    }

    // On: what the owner holds that we have not put there ourselves. A worn flag alone does not count.
    for (const auto& item : desiredItems)
    {
        if (ContainsHandItem(heldItems, item.pForm, item.Left))
            continue;
        // Already in that hand, put there since: equipping it again would empty the hand (`live-equip-twice`).
        if (apActor->GetEquippedWeapon(item.Left ? 0 : 1) == item.pForm)
        {
            NoteHandItem(apActor, item.pForm, item.Left, true);
            continue;
        }
        auto* pObject = Cast<TESBoundObject>(item.pForm);
        if (!pObject)
            continue;
        // The remote copy may not have the item yet (picked up after it was spawned).
        if (apActor->GetItemCountInInventory(item.pForm) <= 0)
        {
            ScopedInventoryOverride _;
            apActor->AddObjectToContainer(pObject, nullptr, 1, nullptr);
        }
        // A bow or a two-handed weapon takes its own both-hands slot; anything else the hand it is held in.
        // Leaving the slot to the game would put a second one-handed weapon in whichever hand is free.
        TESForm* pSlot = item.Left ? defaultObjects.leftEquipSlot : defaultObjects.rightEquipSlot;
        if (const auto* pEquipType = item.Left ? nullptr : Cast<BGSEquipType>(item.pForm))
        {
            auto* pOwnSlot = reinterpret_cast<TESForm*>(pEquipType->unk4); // BGSEquipType::equipSlot
            if (pOwnSlot && pOwnSlot != defaultObjects.eitherEquipSlot && pOwnSlot != defaultObjects.leftEquipSlot && pOwnSlot != defaultObjects.rightEquipSlot)
                pSlot = pOwnSlot;
        }
        spdlog::info("Equipment sync: remote actor {:X} equips {:X} ({} hand{})", apActor->formID, item.pForm->formID, item.Left ? "left" : "right",
                     pSlot != defaultObjects.leftEquipSlot && pSlot != defaultObjects.rightEquipSlot ? ", both hands" : "");
        pEquipManager->Equip(apActor, item.pForm, nullptr, 1, pSlot, false, true, false, false);
        NoteHandItem(apActor, item.pForm, item.Left, true);
    }

    const auto syncSpell = [&](const GameId& acDesiredSpell, uint32_t aHand)
    {
        TESForm* pDesired = acDesiredSpell ? TESForm::GetById(modSystem.GetLocalSpellId(acDesiredSpell)) : nullptr;
        TESForm* pCurrent = apActor->magicItems[aHand];
        if (pDesired == pCurrent)
            return;

        spdlog::info("Equipment sync: remote actor {:X} {} hand spell {:X} -> {:X}", apActor->formID, aHand == 0 ? "left" : "right", pCurrent ? pCurrent->formID : 0, pDesired ? pDesired->formID : 0);

        if (pDesired)
            pEquipManager->EquipSpell(apActor, pDesired, aHand);
        else
            pEquipManager->UnEquipSpell(apActor, pCurrent, aHand);
    };

    syncSpell(acEquipment.CurrentMagicEquipment.LeftHandSpell, 0);
    syncSpell(acEquipment.CurrentMagicEquipment.RightHandSpell, 1);
}
