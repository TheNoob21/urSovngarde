#include "InventoryService.h"

#include <Components.h>
#include <ContainerTrust.h>
#include <EpochDrop.h>
#include <World.h>
#include <GameServer.h>

#include <Messages/NotifyObjectInventoryChanges.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/RequestEquipmentChanges.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/DrawWeaponRequest.h>
#include <Messages/NotifyDrawWeapon.h>
#include <Structs/ItemDataChange.h>

#include <Services/ItemTakeService.h>

#include <Setting.h>
#include <FollowerCommand.h>

InventoryService::InventoryService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_inventoryChangeConnection = aDispatcher.sink<PacketEvent<RequestInventoryChanges>>().connect<&InventoryService::OnInventoryChanges>(this);
    m_equipmentChangeConnection = aDispatcher.sink<PacketEvent<RequestEquipmentChanges>>().connect<&InventoryService::OnEquipmentChanges>(this);
    m_drawWeaponConnection = aDispatcher.sink<PacketEvent<DrawWeaponRequest>>().connect<&InventoryService::OnWeaponDrawnRequest>(this);
}

void InventoryService::OnInventoryChanges(const PacketEvent<RequestInventoryChanges>& acMessage) noexcept
{
    auto& message = acMessage.Packet;

    // A run-time item sent without what it is can be made by no other game.
    if (!ItemDataChanges::IsSyncable(message.Item))
        return;

    auto view = m_world.view<InventoryComponent>();

    const auto it = view.find(static_cast<entt::entity>(message.ServerId));

    if (it == view.end())
        return;

    bool isRemoteNpcInteraction = false;

    const auto* pOwnerComponent = m_world.try_get<OwnerComponent>(*it);
    if (pOwnerComponent)
    {
        const auto* pOwner = pOwnerComponent->GetOwner();
        if (pOwnerComponent->OwnershipEpoch != message.OwnershipEpoch)
        {
            const uint32_t ownerId = pOwner ? pOwner->GetId() : 0;
            spdlog::debug(
                "Rejected inventory change from player {:X} for actor {:X} at stale epoch {}; current owner is {:X} at epoch {}",
                acMessage.pPlayer->GetId(), message.ServerId, message.OwnershipEpoch, ownerId, pOwnerComponent->OwnershipEpoch);
            return;
        }

        if (pOwner != acMessage.pPlayer)
        {
            const auto* pCharacterComponent = m_world.try_get<CharacterComponent>(*it);
            const auto* pCellComponent = m_world.try_get<CellIdComponent>(*it);
            // A non-owner may still change a nearby NPC's inventory by pickpocketing or looting.
            isRemoteNpcInteraction = pOwner && pCharacterComponent && pCellComponent && !pCharacterComponent->IsPlayer()
                && acMessage.pPlayer->GetCellComponent().IsInRange(*pCellComponent, pCharacterComponent->IsDragon());

            if (!isRemoteNpcInteraction)
            {
                const uint32_t ownerId = pOwner ? pOwner->GetId() : 0;
                spdlog::debug(
                    "Rejected inventory change from player {:X} for actor {:X} because it is owned by player {:X}", acMessage.pPlayer->GetId(), message.ServerId, ownerId);
                return;
            }

            // A follower's trade accepts additions from her player's party, as her other commands do.
            // Ordinary non-follower looting and transfers keep their existing range/epoch policy.
            if (message.Item.Count > 0 && pOwnerComponent->FollowerOfPlayerId != 0)
            {
                auto& parties = m_world.GetPartyService();
                Player* pFollowerPlayer = m_world.GetPlayerManager().GetById(pOwnerComponent->FollowerOfPlayerId);
                const auto* pParty = parties.GetPlayerParty(acMessage.pPlayer);
                const bool cSameParty = pParty && pFollowerPlayer && pParty == parties.GetPlayerParty(pFollowerPlayer);
                if (!FollowerCommand::MayAsk(pOwnerComponent->FollowerOfPlayerId, pFollowerPlayer != nullptr, acMessage.pPlayer->GetId(), cSameParty))
                {
                    spdlog::debug("Rejected inventory addition from player {:X} for follower {:X}: not of her player's party", acMessage.pPlayer->GetId(), message.ServerId);
                    return;
                }
            }
        }
    }
    else if (message.OwnershipEpoch != 0)
    {
        spdlog::warn(
            "Rejected inventory change from player {:X} because object {:X} unexpectedly carried ownership epoch {}",
            acMessage.pPlayer->GetId(), message.ServerId, message.OwnershipEpoch);
        return;
    }

    // A container no player owns is changed only from a game that has it in range, by a count it could hold, as the whole-chest path does.
    if (!pOwnerComponent)
    {
        if (const auto* pContainerCell = m_world.try_get<CellIdComponent>(*it))
        {
            const auto* pContainerCharacter = m_world.try_get<CharacterComponent>(*it);
            const bool cInRange =
                acMessage.pPlayer->GetCellComponent() && acMessage.pPlayer->GetCellComponent().IsInRange(*pContainerCell, pContainerCharacter && pContainerCharacter->IsDragon());
            if (!ContainerTrust::MayChange(cInRange, message.Item.Count))
            {
                spdlog::debug(
                    "Rejected inventory change from player {:X} for container {:X}: out of range or an impossible count ({})", acMessage.pPlayer->GetId(), message.ServerId,
                    message.Item.Count);
                return;
            }
        }
    }

    auto& inventoryComponent = view.get<InventoryComponent>(*it);
    Inventory::Entry item = message.Item;

    // A take out of a chest or a body, which another player may have made first: the server gives what it still holds, never below none.
    // Only a chest or a dead body refuses one. A merchant's chest is sent whole after a trade instead (VendorChest), and a living NPC's
    // removals are mostly its own (a potion drunk, an arrow shot) or paid for (barter).
    const auto* pCharacterComponent = m_world.try_get<CharacterComponent>(*it);
    if (item.Count < 0 && !(pCharacterComponent && pCharacterComponent->IsPlayer()))
    {
        const auto* pObjectComponent = m_world.try_get<ObjectComponent>(*it);
        const bool cMayRefuse = pCharacterComponent ? pCharacterComponent->IsDead() : !(pObjectComponent && pObjectComponent->IsVendor);
        item.Count = -m_world.ctx().at<ItemTakeService>().TakeOut(acMessage.pPlayer, message.ServerId, inventoryComponent.Content, message.Item, cMayRefuse);
        if (item.Count == 0)
            return;
    }
    else
        inventoryComponent.Content.AddOrRemoveEntry(item);

    // A shared chest that came empty and is changed now is still a container: the ContainerLedger keeps it when its cell empties.
    if (auto* pObjectComponent = m_world.try_get<ObjectComponent>(*it))
        pObjectComponent->HadContent = true;

    if (!message.UpdateClients && !isRemoteNpcInteraction)
        return;

    NotifyInventoryChanges notify;
    notify.ServerId = message.ServerId;
    notify.OwnershipEpoch = message.OwnershipEpoch;
    notify.Item = item;
    notify.PickedUp = message.PickedUp;

    // Never forwarded as a drop: DroppedItemService places the item, and the flag would drop a second one.
    notify.Drop = false;

    const entt::entity cOrigin = static_cast<entt::entity>(message.ServerId);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cOrigin, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void InventoryService::OnEquipmentChanges(const PacketEvent<RequestEquipmentChanges>& acMessage) noexcept
{
    auto& message = acMessage.Packet;

    auto view = m_world.view<InventoryComponent>();

    const auto it = view.find(static_cast<entt::entity>(message.ServerId));

    if (it == view.end())
        return;

    const auto* pOwnerComponent = m_world.try_get<OwnerComponent>(*it);
    if (pOwnerComponent)
    {
        if (pOwnerComponent->GetOwner() != acMessage.pPlayer || pOwnerComponent->OwnershipEpoch != message.OwnershipEpoch)
        {
            const uint32_t ownerId = pOwnerComponent->GetOwner() ? pOwnerComponent->GetOwner()->GetId() : 0;
            // ReportEpochDrop is the warning that reaches the log file.
            spdlog::debug("Rejected equipment change from player {:X} for actor {:X}; current owner is {:X} and requested epoch {} does not match {}", acMessage.pPlayer->GetId(), message.ServerId,
                          ownerId, message.OwnershipEpoch, pOwnerComponent->OwnershipEpoch);
            ReportEpochDrop("an equipment change", message.ServerId, message.OwnershipEpoch, pOwnerComponent->OwnershipEpoch);
            return;
        }
    }
    else if (message.OwnershipEpoch != 0)
    {
        spdlog::warn(
            "Rejected equipment change from player {:X} because object {:X} unexpectedly carried ownership epoch {}",
            acMessage.pPlayer->GetId(), message.ServerId, message.OwnershipEpoch);
        return;
    }

    auto& inventoryComponent = view.get<InventoryComponent>(*it);
    inventoryComponent.Content.UpdateEquipment(message.CurrentInventory);

    NotifyEquipmentChanges notify;
    notify.ServerId = message.ServerId;
    notify.OwnershipEpoch = message.OwnershipEpoch;
    notify.ItemId = message.ItemId;
    notify.EquipSlotId = message.EquipSlotId;
    notify.Count = message.Count;
    notify.Unequip = message.Unequip;
    notify.IsSpell = message.IsSpell;
    notify.IsShout = message.IsShout;
    notify.CurrentInventory = message.CurrentInventory;

    const entt::entity cOrigin = static_cast<entt::entity>(message.ServerId);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cOrigin, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void InventoryService::OnWeaponDrawnRequest(const PacketEvent<DrawWeaponRequest>& acMessage) noexcept
{
    auto& message = acMessage.Packet;

    auto characterView = m_world.view<CharacterComponent, OwnerComponent>();
    const auto it = characterView.find(static_cast<entt::entity>(message.Id));

    if (it != std::end(characterView) && characterView.get<OwnerComponent>(*it).GetOwner() == acMessage.pPlayer)
    {
        auto& characterComponent = characterView.get<CharacterComponent>(*it);
        characterComponent.SetWeaponDrawn(message.IsWeaponDrawn);
        spdlog::debug("Updating weapon drawn state {:x}:{}", message.Id, message.IsWeaponDrawn);

        // A player's draw reaches the others' copies by its animation, which a paused game refuses; this lets them put it right once they run again.
        if (characterComponent.IsPlayer())
        {
            NotifyDrawWeapon notify;
            notify.Id = message.Id;
            notify.IsWeaponDrawn = message.IsWeaponDrawn;
            if (!GameServer::Get()->SendToPlayersInRange(notify, *it, acMessage.GetSender()))
                spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
        }
    }
}
