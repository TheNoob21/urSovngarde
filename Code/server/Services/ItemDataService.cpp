#include <Services/ItemDataService.h>
#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Messages/RequestItemDataChange.h>
#include <Messages/NotifyItemDataChange.h>

ItemDataService::ItemDataService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_itemDataConnection = aDispatcher.sink<PacketEvent<RequestItemDataChange>>().connect<&ItemDataService::OnItemDataChange>(this);
}

// Only the game that runs a character changes its items in place; a change from anyone else is dropped.
void ItemDataService::OnItemDataChange(const PacketEvent<RequestItemDataChange>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;
    const entt::entity cEntity = static_cast<entt::entity>(message.ServerId);
    if (!m_world.valid(cEntity) || !m_world.all_of<InventoryComponent, OwnerComponent>(cEntity))
        return;

    const auto& ownerComponent = m_world.get<OwnerComponent>(cEntity);
    if (ownerComponent.GetOwner() != acMessage.pPlayer || ownerComponent.OwnershipEpoch != message.OwnershipEpoch)
    {
        spdlog::debug("ItemData: change from player {:X} for actor {:X} dropped, not its owner at epoch {}", acMessage.pPlayer->GetId(), message.ServerId, message.OwnershipEpoch);
        return;
    }

    NotifyItemDataChange notify{};
    notify.ServerId = message.ServerId;
    notify.OwnershipEpoch = message.OwnershipEpoch;

    auto& inventory = m_world.get<InventoryComponent>(cEntity).Content;
    for (const ItemDataChange& change : message.Changes)
    {
        if (!ItemDataChanges::IsSyncable(change.Old) || !ItemDataChanges::IsSyncable(change.New))
            continue;
        if (ItemDataChanges::Apply(inventory, change) == 0)
            spdlog::debug("ItemData: actor {:X} had none of {:X}:{:X} as it was; kept as it is", message.ServerId, change.Old.BaseId.ModId, change.Old.BaseId.BaseId);
        notify.Changes.push_back(change);
    }

    if (notify.Changes.empty())
        return;

    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.GetSender()))
        spdlog::debug("{}: SendToPlayersInRange failed", __FUNCTION__);
}
