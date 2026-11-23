#include <Services/DroppedItemService.h>

#include <ClientTrust.h>
#include <DroppedItemIdentity.h>
#include <DroppedItemTrust.h>
#include <GameServer.h>
#include <World.h>
#include <Components.h>

#include <Messages/RequestDroppedItemAdd.h>
#include <Messages/RequestDroppedItemRemove.h>
#include <Messages/NotifyDroppedItem.h>
#include <Messages/NotifyDroppedItemRemoved.h>
#include <Messages/RequestDroppedItemMove.h>
#include <Messages/NotifyDroppedItemMove.h>
#include <Messages/RequestWorldObjectMove.h>
#include <MoverNearness.h>
#include <Messages/RequestItemTake.h>
#include <Messages/NotifyWorldObjectMove.h>
#include <Messages/RequestPropBodies.h>
#include <Messages/NotifyPropBodies.h>
#include <Events/PlayerLeaveEvent.h>
#include <Events/UpdateEvent.h>
#include <Messages/EnterInteriorCellRequest.h>
#include <Messages/EnterExteriorCellRequest.h>
#include <Messages/ShiftGridCellRequest.h>

#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <Structs/ItemDataChange.h>

#include <fstream>
#include <limits>

namespace
{
// Next to the server, where it is started from. Deleting it forgets every dropped item and moved object's place, and nothing else.
constexpr const char* kFile = "dropped_items.bin";
// Versions 2/3 added rotation/ItemData; version 4 preserves missing legacy ItemData across saves; placed objects follow in a trailer.
constexpr uint64_t kVersion = DroppedItemFile::kInlineObjectsVersion;

//! How close an announced item must lie to a known one of the same stack and place to count as the same item.
//! Generous, because the item may have settled or been reloaded since.
constexpr float kSameItemRadius = 200.f;

CellIdComponent CellOf(const DroppedItemService::Item& acItem) noexcept
{
    return {acItem.CellId, acItem.WorldSpaceId, GridCellCoords::CalculateGridCellCoords(acItem.Position)};
}

/**
 * Check that a player stands near a place in a cell: their loaded grid holds the cell and their character is close (MoverNearness).
 *
 * @param aWorld The server's world, for the player's character.
 * @param apPlayer The player.
 * @param acCell The cell of the place.
 * @param acPosition The place.
 * @param aMaxDistance How far the player's character may be from it (MoverNearness).
 * @return True when the player is near.
 */
bool IsPlayerNear(World& aWorld, Player* apPlayer, const CellIdComponent& acCell, const glm::vec3& acPosition, const float aMaxDistance) noexcept
{
    if (!apPlayer)
        return false;
    const auto& senderCell = apPlayer->GetCellComponent();
    const bool cCellInRange = senderCell && senderCell.WorldSpaceId == acCell.WorldSpaceId && senderCell.IsInRange(acCell, false);
    const auto cCharacter = apPlayer->GetCharacter();
    const auto* pMovement = cCharacter ? aWorld.try_get<MovementComponent>(*cCharacter) : nullptr;
    return MoverNearness::IsNear(cCellInRange, pMovement && pMovement->Tick, pMovement ? pMovement->Position : glm::vec3{}, acPosition, aMaxDistance);
}
} // namespace

DroppedItemService::DroppedItemService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_addConnection(aDispatcher.sink<PacketEvent<RequestDroppedItemAdd>>().connect<&DroppedItemService::OnAdd>(this))
    , m_removeConnection(aDispatcher.sink<PacketEvent<RequestDroppedItemRemove>>().connect<&DroppedItemService::OnRemove>(this))
    , m_moveConnection(aDispatcher.sink<PacketEvent<RequestDroppedItemMove>>().connect<&DroppedItemService::OnMove>(this))
    , m_objectMoveConnection(aDispatcher.sink<PacketEvent<RequestWorldObjectMove>>().connect<&DroppedItemService::OnObjectMove>(this))
    , m_propBodiesConnection(aDispatcher.sink<PacketEvent<RequestPropBodies>>().connect<&DroppedItemService::OnPropBodies>(this))
    , m_leaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&DroppedItemService::OnPlayerLeave>(this))
    , m_interiorConnection(aDispatcher.sink<PacketEvent<EnterInteriorCellRequest>>().connect<&DroppedItemService::OnEnterInteriorCell>(this))
    , m_exteriorConnection(aDispatcher.sink<PacketEvent<EnterExteriorCellRequest>>().connect<&DroppedItemService::OnEnterExteriorCell>(this))
    , m_gridConnection(aDispatcher.sink<PacketEvent<ShiftGridCellRequest>>().connect<&DroppedItemService::OnShiftGridCell>(this))
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&DroppedItemService::OnUpdate>(this))
{
    Load();
}

// The server stops: changes not yet written are written now.
DroppedItemService::~DroppedItemService()
{
    if (m_save.Flush(std::chrono::steady_clock::now()))
        Save();
}

void DroppedItemService::OnUpdate(const UpdateEvent&) noexcept
{
    if (m_save.Due(std::chrono::steady_clock::now()))
        Save();
}

void DroppedItemService::OnAdd(const PacketEvent<RequestDroppedItemAdd>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;

    if (message.Item.BaseId == GameId{} || message.Item.Count == 0 || message.Item.Count == std::numeric_limits<int32_t>::min() || !ItemDataChanges::IsSyncable(message.Item))
        return;

    // A place that is no number, or beyond the world, is not kept: it would go to every late joiner and into the save.
    if (!ClientTrust::IsUsableTransform(message.Position.x, message.Position.y, message.Position.z, message.Rotation.x, message.Rotation.y, message.Rotation.z))
        return;

    // Only a player at the place adds an item there: it is kept for good and sent to everyone who comes near.
    // An announcement may arrive before the game's first cell entry does, so it is held to the sender's loaded grid only once that is known (DroppedItemTrust).
    const CellIdComponent cPlace{message.CellId, message.WorldSpaceId, GridCellCoords::CalculateGridCellCoords(message.Position)};
    const auto& cSenderCell = acMessage.pPlayer->GetCellComponent();
    const bool cCellInRange = cSenderCell && cSenderCell.WorldSpaceId == cPlace.WorldSpaceId && cSenderCell.IsInRange(cPlace, false);
    if (!DroppedItemTrust::MayAdd(
            message.Announcement, static_cast<bool>(cSenderCell), cCellInRange,
            IsPlayerNear(m_world, acMessage.pPlayer, cPlace, static_cast<const glm::vec3&>(message.Position), MoverNearness::kLoadedGridReach)))
    {
        spdlog::debug("DroppedItem: add from player {:X} dropped; the player is not near it", acMessage.pPlayer->GetId());
        return;
    }
    // A game announces its earlier drops in one burst after a load, so the budget holds that; beyond it a player adds a few a second.
    if (!m_addBudgets[acMessage.pPlayer->GetId()].Allow(std::chrono::steady_clock::now()))
    {
        spdlog::debug("DroppedItem: add from player {:X} dropped; the player adds too many", acMessage.pPlayer->GetId());
        return;
    }

    // An announced item may already be known: answering with its id lets the client adopt it instead of doubling it.
    if (message.Announcement)
    {
        const auto cMatch = DroppedItemIdentity::Find(m_items, message.Item, message.CellId, message.WorldSpaceId, message.Position, kSameItemRadius);
        if (cMatch.Ambiguous)
        {
            spdlog::debug("DroppedItem: legacy announcement matches multiple saved stacks; keeping them until identified");
            return;
        }
        if (cMatch.Id)
        {
            auto& known = m_items.at(cMatch.Id);
            if (known.EnrichItemData(message.Item.Data))
            {
                m_save.Mark();
                const CellIdComponent cell = CellOf(known);
                for (Player* pPlayer : m_world.GetPlayerManager())
                    if (pPlayer != acMessage.pPlayer && pPlayer->GetCellComponent().IsInRange(cell, false))
                        SendItem(pPlayer, known, 0, true);
            }
            SendItem(acMessage.pPlayer, known, message.RequestId, true);
            return;
        }
    }

    // A full list forgets its oldest item rather than refuse a new one: nothing else ever ends a dropped item. Games that still hold the forgotten one keep it.
    if (const size_t cForgotten = DroppedItemTrust::ForgetOldest(m_items, DroppedItemTrust::kMaxItems))
        spdlog::warn("DroppedItem: the list holds {} items; the {} oldest are forgotten to make room", DroppedItemTrust::kMaxItems, cForgotten);

    Item item{};
    item.Id = m_nextId++;
    item.Entry = message.Item;
    // A drop arrives as a negative inventory change; the item on the floor is a count.
    item.Entry.Count = std::abs(item.Entry.Count);
    item.CellId = message.CellId;
    item.WorldSpaceId = message.WorldSpaceId;
    item.Position = message.Position;
    item.RestPosition = message.Position;
    item.Rotation = message.Rotation;

    const auto [it, inserted] = m_items.emplace(item.Id, item);
    m_save.Mark();

    spdlog::info("Dropped item {} registered: {:X}:{:X} x{} by player {:X}{}, cell {:X}, worldspace {:X}, at ({:.0f})", item.Id,
                 item.Entry.BaseId.ModId, item.Entry.BaseId.BaseId, item.Entry.Count, acMessage.pPlayer->GetId(),
                 message.Announcement ? " (announced from an earlier drop)" : "", item.CellId.BaseId, item.WorldSpaceId.BaseId, item.Position);

    // Sent to the dropper as well, so its client learns the item's id, with its request id; nobody else gets that.
    const CellIdComponent cell = CellOf(it->second);
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer == acMessage.pPlayer)
            SendItem(pPlayer, it->second, message.RequestId, message.Announcement);
        else if (pPlayer->GetCellComponent().IsInRange(cell, false))
            SendItem(pPlayer, it->second, 0, message.Announcement);
    }
}

void DroppedItemService::OnRemove(const PacketEvent<RequestDroppedItemRemove>& acMessage) noexcept
{
    const auto it = m_items.find(acMessage.Packet.Id);
    if (it == m_items.end())
        return; // somebody else picked it up first, and the removal is already on its way

    // Only a player near where the item lies picks it up: the removal is kept for good and tells every game to delete it.
    if (!DroppedItemTrust::MayRemove(
            IsPlayerNear(m_world, acMessage.pPlayer, CellOf(it->second), static_cast<const glm::vec3&>(it->second.Position), MoverNearness::kLoadedGridReach)))
    {
        spdlog::debug("DroppedItem: removal of {} from player {:X} dropped; the player is not near it", it->first, acMessage.pPlayer ? acMessage.pPlayer->GetId() : 0);
        return;
    }

    spdlog::info("Dropped item {} picked up by player {:X}", it->first, acMessage.pPlayer->GetId());
    m_items.erase(it);
    m_itemMovers.erase(acMessage.Packet.Id);
    m_save.Mark();

    // Sent to everyone, not only those in range: a player out of range may still have it in their save.
    NotifyDroppedItemRemoved notify{};
    notify.Id = acMessage.Packet.Id;
    for (Player* pPlayer : m_world.GetPlayerManager())
        pPlayer->Send(notify);
}

void DroppedItemService::OnMove(const PacketEvent<RequestDroppedItemMove>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;
    if (!ClientTrust::IsUsableTransform(message.Position.x, message.Position.y, message.Position.z, message.Rotation.x, message.Rotation.y, message.Rotation.z))
        return;
    const auto it = m_items.find(message.Id);
    if (it == m_items.end())
        return; // picked up meanwhile
    // Only a player whose game has the item loaded moves it, and only within that grid: the move is kept across restarts (MoverNearness).
    if (!IsPlayerNear(m_world, acMessage.pPlayer, CellOf(it->second), static_cast<const glm::vec3&>(message.Position), MoverNearness::kLoadedGridReach))
    {
        spdlog::debug("Dropped item {}: move from player {:X} dropped; the player is not near it", message.Id, acMessage.pPlayer ? acMessage.pPlayer->GetId() : 0);
        return;
    }
    const auto cNow = std::chrono::steady_clock::now();
    ForgetLapsedMovers(m_itemMovers, cNow, kMoversKept);
    // The first move after a pause goes on reliable, as the rest does; the moves in between go unreliable, as they came.
    const auto cMover = m_itemMovers.find(message.Id);
    const bool cStart = cMover == m_itemMovers.end() || cMover->second.IsLapsed(cNow);
    // A put-down always counts; the moves before it are capped for each player, as hinged bodies are.
    const uint32_t cPlayerId = acMessage.pPlayer->GetId();
    if (!TakeCapped(m_itemMovers, message.Id, m_moveRates[cPlayerId], cPlayerId, message.AtRest, cNow, message.Tick))
        return; // another player is moving it, it was sent before the last move passed on, or the player sends too many

    it->second.Position = message.Position;
    it->second.Rotation = message.Rotation;
    it->second.Fixed = message.AtRest && message.Fixed;
    // Put down far from where it rested: that place is kept, for a game whose save still has it there.
    if (message.AtRest)
        it->second.NoteRest(message.Position, kSameItemRadius);

    // Written to disk only once it stops, not on every update of a held item.
    if (message.AtRest)
        m_save.Mark();

    NotifyDroppedItemMove notify{};
    notify.Id = message.Id;
    notify.Position = message.Position;
    notify.Rotation = message.Rotation;
    notify.AtRest = message.AtRest;
    notify.Fixed = it->second.Fixed;
    // The sender's own sample time goes on as it is: only the receiver knows what to make of it.
    notify.Tick = message.Tick;
    notify.MoverPlayerId = cPlayerId;
    notify.Thrown = message.Thrown;
    notify.Velocity = message.Velocity;
    notify.Spin = message.Spin;

    const auto cFlags = message.AtRest || cStart ? TiltedPhoques::kReliable : TiltedPhoques::kUnreliable;
    const CellIdComponent cell = CellOf(it->second);
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer != acMessage.pPlayer && pPlayer->GetCellComponent().IsInRange(cell, false))
            pPlayer->Send(notify, cFlags);
    }
}

// A placed world object moved by a player's hand: relayed to everyone in range, and remembered for anyone who comes near later.
void DroppedItemService::OnObjectMove(const PacketEvent<RequestWorldObjectMove>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;
    if (message.Taken)
        return;
    if (!ClientTrust::IsUsableTransform(message.Position.x, message.Position.y, message.Position.z, message.Rotation.x, message.Rotation.y, message.Rotation.z))
        return;
    // A placed object is a reference of a loaded plugin, in a cell.
    if (!acMessage.pPlayer || !message.ObjectId.BaseId || !message.CellId.BaseId)
        return;
    // Only a player near the object moves it: the move is kept and replayed to everyone who comes near later (MoverNearness).
    const CellIdComponent cell{message.CellId, message.WorldSpaceId, GridCellCoords::CalculateGridCellCoords(message.Position)};
    if (!IsPlayerNear(m_world, acMessage.pPlayer, cell, static_cast<const glm::vec3&>(message.Position), MoverNearness::kMaxDistance))
    {
        spdlog::debug(
            "WorldObject: move of {:X}:{:X} from player {:X} dropped; the player is not near it", message.ObjectId.ModId, message.ObjectId.BaseId, acMessage.pPlayer->GetId());
        return;
    }
    const uint64_t cKey = message.ObjectId.LogFormat();
    if (const auto it = m_objects.find(cKey); it != m_objects.end() && it->second.Taken)
        return;
    const auto cNow = std::chrono::steady_clock::now();
    ForgetLapsedMovers(m_objectMovers, cNow, kMoversKept);
    const uint32_t cPlayerId = acMessage.pPlayer->GetId();
    if (TakeCapped(m_objectMovers, cKey, m_moveRates[cPlayerId], cPlayerId, message.AtRest, cNow, message.Tick))
        RecordObjectMove(message, acMessage.pPlayer);
}

void DroppedItemService::RecordPlacedTake(const RequestItemTake& acMessage, Player* apPlayer) noexcept
{
    const uint64_t cKey = acMessage.ObjectId.LogFormat();
    if (const auto it = m_objects.find(cKey); it != m_objects.end() && it->second.Taken)
        return;
    m_objectMovers.erase(cKey);
    m_bodyMovers.erase(cKey);
    RequestWorldObjectMove taken{};
    taken.ObjectId = acMessage.ObjectId;
    taken.CellId = acMessage.CellId;
    taken.WorldSpaceId = acMessage.WorldSpaceId;
    taken.Position = acMessage.Position;
    taken.Taken = true;
    taken.AtRest = true;
    RecordObjectMove(taken, apPlayer);
}

void DroppedItemService::RecordObjectMove(const RequestWorldObjectMove& acMessage, Player* apPlayer) noexcept
{
    const uint64_t cKey = acMessage.ObjectId.LogFormat();
    const auto cNow = std::chrono::steady_clock::now();
    MakeRoom(m_objects, kMaxObjects, cKey, [](const ObjectPlace& acPlace) { return acPlace.LastSeen; });
    ObjectPlace& place = m_objects[cKey];
    // A move after a rest or a pause starts a carry and goes on reliable, as a rest and a take do; the moves in between go unreliable, as they came.
    const bool cStart = place.MovedAt == std::chrono::steady_clock::time_point{} || cNow - place.MovedAt >= ObjectMover::kHeldFor;
    place = ObjectPlace{acMessage.ObjectId, acMessage.CellId, acMessage.WorldSpaceId, acMessage.Position, acMessage.Rotation, acMessage.Taken};
    place.LastSeen = cNow;
    if (!acMessage.AtRest && !acMessage.Taken)
        place.MovedAt = cNow;

    // A new resting place outlives a server restart; a take lasts for the session only.
    if (acMessage.AtRest && !acMessage.Taken)
        m_save.Mark();

    NotifyWorldObjectMove notify{};
    notify.ObjectId = acMessage.ObjectId;
    notify.CellId = acMessage.CellId;
    notify.WorldSpaceId = acMessage.WorldSpaceId;
    notify.Position = acMessage.Position;
    notify.Rotation = acMessage.Rotation;
    notify.AtRest = acMessage.AtRest;
    notify.Taken = acMessage.Taken;
    notify.Tick = acMessage.Tick;
    notify.MoverPlayerId = apPlayer ? apPlayer->GetId() : 0;
    notify.Thrown = acMessage.Thrown;
    notify.Velocity = acMessage.Velocity;
    notify.Spin = acMessage.Spin;

    const auto cFlags = acMessage.AtRest || acMessage.Taken || cStart ? TiltedPhoques::kReliable : TiltedPhoques::kUnreliable;
    const CellIdComponent cell{acMessage.CellId, acMessage.WorldSpaceId, GridCellCoords::CalculateGridCellCoords(acMessage.Position)};
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer != apPlayer && pPlayer->GetCellComponent().IsInRange(cell, false))
            pPlayer->Send(notify, cFlags);
    }
}

// A placed object's hinged bodies swung by a player: relayed to everyone in range, one player's swing at a time.
// Not remembered: hanging bodies settle by themselves in every game.
void DroppedItemService::OnPropBodies(const PacketEvent<RequestPropBodies>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;
    if (!acMessage.pPlayer || !message.ObjectId || !message.CellId || !message.Bodies.Layout || message.Bodies.Bodies.empty())
        return;
    const CellIdComponent cell{message.CellId, message.WorldSpaceId, GridCellCoords::CalculateGridCellCoords(message.Position)};
    const auto& senderCell = acMessage.pPlayer->GetCellComponent();
    if (!senderCell || senderCell.WorldSpaceId != cell.WorldSpaceId || !senderCell.IsInRange(cell, false))
        return;
    const auto cCharacter = acMessage.pPlayer->GetCharacter();
    const auto* pMovement = cCharacter ? m_world.try_get<MovementComponent>(*cCharacter) : nullptr;
    constexpr float cMaxSenderDistance = 2048.f;
    if (!pMovement || !pMovement->Tick || !(glm::distance(pMovement->Position, static_cast<const glm::vec3&>(message.Position)) <= cMaxSenderDistance))
        return;

    const uint64_t cKey = message.ObjectId.LogFormat();
    if (const auto place = m_objects.find(cKey);
        place != m_objects.end() && (place->second.Taken || place->second.CellId != cell.Cell || place->second.WorldSpaceId != cell.WorldSpaceId))
        return;

    const auto cNow = std::chrono::steady_clock::now();
    const uint32_t cPlayerId = acMessage.pPlayer->GetId();
    auto mover = m_bodyMovers.find(cKey);
    // A new swing goes on reliable, as its rest does; the poses in between go unreliable, as they came.
    const bool cStart = mover == m_bodyMovers.end() || mover->second.Mover.IsLapsed(cNow);
    if (mover == m_bodyMovers.end())
    {
        if (message.AtRest)
            return;
        if (m_bodyMovers.size() >= PropBodyRelay::kMaxActive)
            std::erase_if(m_bodyMovers, [cNow](const auto& acMover) { return acMover.second.Mover.IsLapsed(cNow); });
        if (m_bodyMovers.size() >= PropBodyRelay::kMaxActive)
            return;
        PropBodyRelay relay{};
        if (!relay.Accept(cPlayerId, cell.Cell, cell.WorldSpaceId, false, message.Tick, cNow, m_bodyRates[cPlayerId]))
            return;
        m_bodyMovers.emplace(cKey, relay);
    }
    else if (!mover->second.Accept(cPlayerId, cell.Cell, cell.WorldSpaceId, message.AtRest, message.Tick, cNow, m_bodyRates[cPlayerId]))
        return;

    NotifyPropBodies notify{};
    notify.ObjectId = message.ObjectId;
    notify.CellId = message.CellId;
    notify.WorldSpaceId = message.WorldSpaceId;
    notify.Position = message.Position;
    notify.Bodies = message.Bodies;
    notify.AtRest = message.AtRest;
    notify.Tick = message.Tick;

    const auto cFlags = message.AtRest || cStart ? TiltedPhoques::kReliable : TiltedPhoques::kUnreliable;
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer != acMessage.pPlayer && pPlayer->GetCellComponent().WorldSpaceId == cell.WorldSpaceId && pPlayer->GetCellComponent().IsInRange(cell, false))
            pPlayer->Send(notify, cFlags);
    }
}

// A player who leaves swings nothing any more.
void DroppedItemService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    if (!acEvent.pPlayer)
        return;
    const uint32_t cId = acEvent.pPlayer->GetId();
    std::erase_if(m_bodyMovers, [cId](const auto& acMover) { return acMover.second.Mover.GetPlayerId() == cId; });
    std::erase_if(m_objectMovers, [cId](const auto& acMover) { return acMover.second.GetPlayerId() == cId; });
    std::erase_if(m_itemMovers, [cId](const auto& acMover) { return acMover.second.GetPlayerId() == cId; });
    m_bodyRates.erase(cId);
    m_moveRates.erase(cId);
    m_addBudgets.erase(cId);
    m_sent.erase(cId);
    // The last player out ends the session, as for the item takes: a later save may still hold what was taken here.
    if (m_world.GetPlayerManager().Count() <= 1)
    {
        std::erase_if(m_objects, [](const auto& acObject) { return acObject.second.Taken; });
        if (m_save.Flush(std::chrono::steady_clock::now()))
            Save();
    }
}

// Reads the packet, not the player's cell component, which another service updates from the same packet in no set order.
void DroppedItemService::OnEnterInteriorCell(const PacketEvent<EnterInteriorCellRequest>& acMessage) const noexcept
{
    // Indoors everything there is sent afresh.
    m_sent.erase(acMessage.pPlayer->GetId());
    SendInRange(acMessage.pPlayer, CellIdComponent{acMessage.Packet.CellId});
}

void DroppedItemService::OnEnterExteriorCell(const PacketEvent<EnterExteriorCellRequest>& acMessage) const noexcept
{
    const auto& message = acMessage.Packet;
    SendInRange(acMessage.pPlayer, CellIdComponent{message.CellId, message.WorldSpaceId, message.CurrentCoords});
}

void DroppedItemService::OnShiftGridCell(const PacketEvent<ShiftGridCellRequest>& acMessage) const noexcept
{
    const auto& message = acMessage.Packet;
    // The same centre again is the player's game announcing its cells again, having lost what it was sent: everything goes again.
    ViewerSends& sent = m_sent[acMessage.pPlayer->GetId()];
    if (ViewerResends::IsReannounce(sent.ShiftWorldSpaceId, sent.ShiftCenter, message.WorldSpaceId, message.CenterCoords))
    {
        sent.Items.clear();
        sent.Objects.clear();
    }
    sent.ShiftWorldSpaceId = message.WorldSpaceId;
    sent.ShiftCenter = message.CenterCoords;
    SendInRange(acMessage.pPlayer, CellIdComponent{message.PlayerCell, message.WorldSpaceId, message.CenterCoords});
}

void DroppedItemService::SendInRange(Player* apPlayer, const CellIdComponent& acViewer) const noexcept
{
    // What the player had in range before and was sent unchanged, near enough for its game to take it, is not sent again (ViewerResends).
    // A game ignores a far item, and an object whose cell's 3D is not there yet, so those go again once the player comes near.
    ViewerSends& sent = m_sent[apPlayer->GetId()];
    const CellIdComponent cPrevious{sent.Cell, sent.WorldSpaceId, sent.CenterCoords};
    if (acViewer.IsInInteriorCell() || cPrevious.WorldSpaceId != acViewer.WorldSpaceId)
    {
        sent.Items.clear();
        sent.Objects.clear();
    }
    sent.Cell = acViewer.Cell;
    sent.WorldSpaceId = acViewer.WorldSpaceId;
    sent.CenterCoords = acViewer.CenterCoords;

    const auto cCharacter = apPlayer->GetCharacter();
    const auto* pMovement = cCharacter ? m_world.try_get<MovementComponent>(*cCharacter) : nullptr;
    const auto isNear = [&](const glm::vec3& acPosition)
    {
        return acViewer.IsInInteriorCell() || !pMovement || glm::distance(pMovement->Position, acPosition) <= ViewerResends::kNearDistance;
    };

    const auto cNow = std::chrono::steady_clock::now();
    std::map<uint32_t, ViewerResends::Record> items;
    for (const auto& [id, item] : m_items)
    {
        const CellIdComponent cCell = CellOf(item);
        if (!acViewer.IsInRange(cCell, false))
            continue;

        uint64_t stamp = ViewerResends::MixFloat(ViewerResends::MixFloat(0, item.Position.x), item.Position.y);
        stamp = ViewerResends::MixFloat(ViewerResends::MixFloat(stamp, item.Position.z), item.Rotation.x);
        stamp = ViewerResends::MixFloat(ViewerResends::MixFloat(stamp, item.Rotation.y), item.Rotation.z);
        stamp = ViewerResends::Mix(ViewerResends::Mix(stamp, static_cast<uint32_t>(item.Entry.Count)), item.LegacyItemData);

        const auto cRecord = sent.Items.find(id);
        const ViewerResends::Record* pRecord = cRecord != sent.Items.end() ? &cRecord->second : nullptr;
        const bool cWasInRange = cPrevious.IsInRange(cCell, false);
        const bool cNear = isNear(item.Position);
        if (!ViewerResends::IsNeeded(pRecord, stamp, cWasInRange, cNear, 2, cNow))
        {
            items[id] = *pRecord;
            continue;
        }
        SendItem(apPlayer, item, 0, true);
        items[id] = ViewerResends::AfterSend(pRecord, stamp, cWasInRange, cNear, cNow);
    }
    sent.Items = std::move(items);

    // A resend is a put-down: sent while the object moves, it stopped a rolling cart where it was and its mover's game put it back there.
    // Its moves reach everyone in range anyway, so it is resent only once it rests, or once its moves have stopped coming.
    constexpr auto cMoveGap = std::chrono::seconds(3);
    std::map<uint64_t, ViewerResends::Record> objects;
    for (const auto& [key, place] : m_objects)
    {
        const CellIdComponent cell{place.CellId, place.WorldSpaceId, GridCellCoords::CalculateGridCellCoords(place.Position)};
        if (!acViewer.IsInRange(cell, false))
            continue;

        const auto cRecord = sent.Objects.find(key);
        const ViewerResends::Record* pRecord = cRecord != sent.Objects.end() ? &cRecord->second : nullptr;
        if (place.MovedAt != std::chrono::steady_clock::time_point{} && cNow - place.MovedAt < cMoveGap)
        {
            if (pRecord)
                objects[key] = *pRecord;
            continue;
        }

        uint64_t stamp = ViewerResends::MixFloat(ViewerResends::MixFloat(0, place.Position.x), place.Position.y);
        stamp = ViewerResends::MixFloat(ViewerResends::MixFloat(stamp, place.Position.z), place.Rotation.x);
        stamp = ViewerResends::MixFloat(ViewerResends::MixFloat(stamp, place.Rotation.y), place.Rotation.z);
        stamp = ViewerResends::Mix(stamp, place.Taken);

        const bool cWasInRange = cPrevious.IsInRange(cell, false);
        const bool cNear = isNear(place.Position);
        if (!ViewerResends::IsNeeded(pRecord, stamp, cWasInRange, cNear, 2, cNow))
        {
            objects[key] = *pRecord;
            continue;
        }
        objects[key] = ViewerResends::AfterSend(pRecord, stamp, cWasInRange, cNear, cNow);

        NotifyWorldObjectMove notify{};
        notify.ObjectId = place.ObjectId;
        notify.CellId = place.CellId;
        notify.WorldSpaceId = place.WorldSpaceId;
        notify.Position = place.Position;
        notify.Rotation = place.Rotation;
        notify.AtRest = true;
        notify.Taken = place.Taken;
        apPlayer->Send(notify);
    }
    sent.Objects = std::move(objects);
}

void DroppedItemService::SendItem(Player* apPlayer, const Item& acItem, const uint32_t aRequestId, const bool aReplay) const noexcept
{
    NotifyDroppedItem notify{};
    notify.Id = acItem.Id;
    notify.RequestId = aRequestId;
    notify.LegacyItemData = acItem.LegacyItemData;
    notify.Replay = aReplay;
    notify.Item = acItem.Entry;
    notify.CellId = acItem.CellId;
    notify.WorldSpaceId = acItem.WorldSpaceId;
    notify.Position = acItem.Position;
    notify.Rotation = acItem.Rotation;
    notify.Fixed = acItem.Fixed;
    notify.EarlierPlaces.assign(acItem.EarlierPlaces.begin(), acItem.EarlierPlaces.end());
    apPlayer->Send(notify);
}

void DroppedItemService::Save() const noexcept
{
    std::vector<DroppedItemRecord> items;
    items.reserve(m_items.size());
    for (const auto& [id, item] : m_items)
        items.push_back(item);
    std::vector<PlacedObjectRecord> objects;
    for (const auto& [key, place] : m_objects)
    {
        if (!place.Taken)
            objects.push_back({place.ObjectId, place.CellId, place.WorldSpaceId, place.Position, place.Rotation});
    }

    TiltedPhoques::Buffer buffer(64 + items.size() * (256 + DroppedItemRecord::kMaxEarlierPlaces * 16) + std::min<size_t>(objects.size(), DroppedItemFile::kMaxObjects) * 64);
    TiltedPhoques::Buffer::Writer writer(&buffer);
    DroppedItemFile::Write(writer, m_nextId, items, objects);

    // Written aside and swapped in, so a crash mid-write leaves the previous list rather than half of one.
    const std::string temporary = std::string(kFile) + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            spdlog::error("Could not write {}: dropped items will not survive a server restart", temporary);
            return;
        }
        file.write(reinterpret_cast<const char*>(buffer.GetData()), static_cast<std::streamsize>(writer.Size()));
    }
    std::error_code error;
    std::filesystem::rename(temporary, kFile, error);
    if (error)
        spdlog::error("Could not replace {}: {}", kFile, error.message());
}

void DroppedItemService::Load() noexcept
{
    std::ifstream file(kFile, std::ios::binary);
    if (!file)
        return; // nothing dropped yet

    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() < 8)
        return;

    TiltedPhoques::ViewBuffer buffer(reinterpret_cast<uint8_t*>(const_cast<char*>(bytes.data())), bytes.size());
    TiltedPhoques::Buffer::Reader reader(&buffer);

    const DroppedItemFile::Contents cContents = DroppedItemFile::Read(reader);
    if (!cContents.Valid)
    {
        // A file with an unknown layout is not read: guessing could place nonsense in every world.
        spdlog::warn("{} is not a list of dropped items this server can read (version {}, up to {} known); starting without it", kFile, cContents.Version, kVersion);
        return;
    }

    m_nextId = cContents.NextId;
    for (const Item& cItem : cContents.Items)
    {
        m_nextId = std::max(m_nextId, cItem.Id + 1);
        // A run-time item kept before it could be told to other games is one no game can place.
        if (!ItemDataChanges::IsSyncable(cItem.Entry))
            continue;
        m_items.emplace(cItem.Id, cItem);
    }
    for (const PlacedObjectRecord& cObject : cContents.Objects)
    {
        if (m_objects.size() >= kMaxObjects)
            break;
        if (cObject.ObjectId == GameId{} || cObject.CellId == GameId{} ||
            !ClientTrust::IsUsableTransform(cObject.Position.x, cObject.Position.y, cObject.Position.z, cObject.Rotation.x, cObject.Rotation.y, cObject.Rotation.z))
            continue;
        m_objects[cObject.ObjectId.LogFormat()] = ObjectPlace{cObject.ObjectId, cObject.CellId, cObject.WorldSpaceId, cObject.Position, cObject.Rotation, false};
    }

    spdlog::info("Loaded {} dropped item(s) and {} moved object place(s) from {}", m_items.size(), m_objects.size(), kFile);
}
