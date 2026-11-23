#include <Services/ItemTakeService.h>
#include <Services/TransportService.h>

#include <Events/DisconnectedEvent.h>
#include <Events/ItemTakeEvent.h>

#include <Messages/RequestItemTake.h>
#include <Messages/NotifyItemTakeLost.h>

#include <World.h>
#include <PlayerCharacter.h>

#include <limits>

ItemTakeService::ItemTakeService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_takeConnection = aDispatcher.sink<ItemTakeEvent>().connect<&ItemTakeService::OnItemTake>(this);
    m_lostConnection = aDispatcher.sink<NotifyItemTakeLost>().connect<&ItemTakeService::OnNotifyItemTakeLost>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&ItemTakeService::OnDisconnected>(this);
}

void ItemTakeService::OnItemTake(const ItemTakeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    // A take out of a container or a body is already on its way, as its removal (InventoryService).
    if (acEvent.From != ItemTakeSource::kInventory)
    {
        // A run-time reference is this game's own: no other game can take it.
        const bool cPlaced = acEvent.From == ItemTakeSource::kPlacedItem;
        if (acEvent.Item.IsQuestItem || (cPlaced && (!acEvent.ObjectId || acEvent.ObjectId.ModId == std::numeric_limits<uint32_t>::max())))
            return;

        RequestItemTake request{};
        request.From = acEvent.From;
        request.Id = acEvent.Id;
        request.ObjectId = acEvent.ObjectId;
        request.CellId = acEvent.CellId;
        request.WorldSpaceId = acEvent.WorldSpaceId;
        request.Position = acEvent.Position;
        m_transport.Send(request);
    }

    const uint64_t cId = acEvent.From == ItemTakeSource::kPlacedItem ? acEvent.ObjectId.LogFormat() : acEvent.Id;
    m_takes.Note(acEvent.From, cId, acEvent.Item, TakeUndo::Clock::now());
}

void ItemTakeService::OnNotifyItemTakeLost(const NotifyItemTakeLost& acMessage) noexcept
{
    const uint64_t cId = acMessage.From == ItemTakeSource::kPlacedItem ? acMessage.ObjectId.LogFormat() : acMessage.Id;
    const auto cBack = m_takes.Lost(acMessage.From, cId, acMessage.Item, TakeUndo::Clock::now());
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!cBack || !pPlayer)
        return;

    // Not under ScopedInventoryOverride: the server and the other games' copies of this player lose it too.
    Inventory::Entry removal = *cBack;
    removal.Count = -removal.Count;
    pPlayer->AddOrRemoveItem(removal);

    spdlog::debug(
        "ItemTake: another player took {:X}:{:X} x{} first (source {}); taken back off the player", cBack->BaseId.ModId, cBack->BaseId.BaseId, cBack->Count,
        static_cast<int>(acMessage.From));
}

void ItemTakeService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_takes.Clear();
}
