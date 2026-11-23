#include <Services/ItemTakeService.h>

#include <GameServer.h>
#include <World.h>
#include <Components.h>
#include <Services/DroppedItemService.h>

#include <Events/PlayerLeaveEvent.h>

#include <Messages/RequestItemTake.h>
#include <Messages/NotifyItemTakeLost.h>

#include <limits>

ItemTakeService::ItemTakeService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_itemTakeConnection(aDispatcher.sink<PacketEvent<RequestItemTake>>().connect<&ItemTakeService::OnItemTake>(this))
    , m_playerLeaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&ItemTakeService::OnPlayerLeave>(this))
{
}

int32_t ItemTakeService::TakeOut(Player* apPlayer, const uint32_t aServerId, Inventory& aContent, const Inventory::Entry& acItem, const bool aMayRefuse) noexcept
{
    const auto cNow = ItemTakes::Clock::now();
    const auto cKey = ItemTakes::Stack(aServerId, acItem.BaseId);
    const int32_t cHeld = ItemTakes::Held(aContent, acItem.BaseId);
    const bool cRaced = aMayRefuse && m_stacks.TakenByAnother(cKey, apPlayer->GetId(), cNow);
    const ItemTakes::Split cSplit = ItemTakes::Decide(cHeld, -acItem.Count, cRaced, acItem.IsQuestItem);
    const auto [cGiven, cRefused] = cSplit;

    ItemTakes::Remove(aContent, acItem, cGiven);
    if (ItemTakes::MarksStack(cSplit, cHeld))
        m_stacks.Note(cKey, apPlayer->GetId(), cNow);

    if (cRefused > 0)
    {
        NotifyItemTakeLost notify{};
        notify.From = ItemTakeSource::kInventory;
        notify.Id = aServerId;
        notify.Item = acItem;
        notify.Item.Count = cRefused;
        apPlayer->Send(notify);
        spdlog::debug(
            "ItemTake: {} of {:X}:{:X} out of {:X} went to another player first; player {:X} gives them back", cRefused, acItem.BaseId.ModId, acItem.BaseId.BaseId, aServerId,
            apPlayer->GetId());
    }

    return cGiven;
}

// Only a pick-up claims: a game also removes a dropped item it lost without one (DroppedItemService), and that is nobody's take.
// The removal itself is DroppedItemService's.
void ItemTakeService::OnItemTake(const PacketEvent<RequestItemTake>& acMessage) noexcept
{
    const auto& cMessage = acMessage.Packet;
    const uint32_t cPlayerId = acMessage.pPlayer->GetId();
    const auto cNow = ItemTakes::Clock::now();

    NotifyItemTakeLost notify{};
    notify.From = cMessage.From;
    if (cMessage.From == ItemTakeSource::kDroppedItem)
    {
        if (m_droppedItems.Claim(cMessage.Id, cPlayerId, cNow))
            return;
        notify.Id = cMessage.Id;
    }
    else if (cMessage.From == ItemTakeSource::kPlacedItem)
    {
        // A run-time reference is one game's own, never shared.
        if (!cMessage.ObjectId || cMessage.ObjectId.ModId == std::numeric_limits<uint32_t>::max())
            return;

        // Only from a game that can reach it: indoors, the cell its player is in.
        const CellIdComponent& cCell = acMessage.pPlayer->GetCellComponent();
        const CellIdComponent cTarget{cMessage.CellId, cMessage.WorldSpaceId, GridCellCoords::CalculateGridCellCoords(cMessage.Position)};
        if (!cTarget || cCell.WorldSpaceId != cTarget.WorldSpaceId || !cCell.IsInRange(cTarget, false))
            return;

        if (m_placedItems.Claim(ItemTakes::Placed(cMessage.ObjectId), cPlayerId, cNow))
        {
            m_world.ctx().at<DroppedItemService>().RecordPlacedTake(cMessage, acMessage.pPlayer);
            return;
        }
        notify.ObjectId = cMessage.ObjectId;
    }
    else
        return;

    acMessage.pPlayer->Send(notify);
    spdlog::debug(
        "ItemTake: item {} ({:X}:{:X}, source {}) went to another player first; player {:X} gives it back", cMessage.Id, cMessage.ObjectId.ModId, cMessage.ObjectId.BaseId,
        static_cast<int>(cMessage.From), cPlayerId);
}

void ItemTakeService::OnPlayerLeave(const PlayerLeaveEvent&) noexcept
{
    // The last player out ends the session, as for the ContainerLedger. The leaver is still counted here.
    if (m_world.GetPlayerManager().Count() > 1)
        return;

    m_stacks.Clear();
    m_droppedItems.Clear();
    m_placedItems.Clear();
}
