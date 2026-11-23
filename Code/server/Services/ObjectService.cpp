#include <Services/ObjectService.h>

#include <GameServer.h>
#include <World.h>
#include <Components.h>

#include <Events/CharacterRemoveEvent.h>
#include <Events/PlayerLeaveCellEvent.h>
#include <Events/PlayerLeaveEvent.h>

#include <Messages/ActivateRequest.h>
#include <Messages/NotifyActivate.h>
#include <Messages/LockChangeRequest.h>
#include <Messages/NotifyLockChange.h>
#include <Messages/AssignObjectsRequest.h>
#include <Messages/AssignObjectsResponse.h>
#include <Messages/ScriptAnimationRequest.h>
#include <Messages/NotifyScriptAnimation.h>
#include <Messages/RequestReferenceEnable.h>
#include <Messages/NotifyReferenceEnable.h>
#include <Messages/RequestObjectInventoryChanges.h>
#include <Messages/NotifyObjectInventoryChanges.h>
#include <Messages/RequestTrapFire.h>
#include <Messages/NotifyTrapFire.h>
#include <Services/PartyService.h>
#include <Services/ActivationService.h>
#include <SharedHomes.h>
#include <Structs/DoorState.h>

namespace
{
// A door's state after the last activation the server passed on, kept apart from the per-cell object entities.
// Those are made only from the opener's own cell and go when that cell empties, so a door opened from the next cell over was never kept.
struct DoorRecord
{
    uint8_t State{};
    //! Where its opener stood; the record goes once no player has that place in range.
    CellIdComponent Where{};
};
Map<GameId, DoorRecord> s_doorStates;
} // namespace

ObjectService::ObjectService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_leaveCellConnection = aDispatcher.sink<PlayerLeaveCellEvent>().connect<&ObjectService::OnPlayerLeaveCellEvent>(this);
    m_playerLeaveConnection = aDispatcher.sink<PlayerLeaveEvent>().connect<&ObjectService::OnPlayerLeave>(this);
    m_assignObjectConnection = aDispatcher.sink<PacketEvent<AssignObjectsRequest>>().connect<&ObjectService::OnAssignObjectsRequest>(this);
    m_activateConnection = aDispatcher.sink<PacketEvent<ActivateRequest>>().connect<&ObjectService::OnActivate>(this);
    m_lockChangeConnection = aDispatcher.sink<PacketEvent<LockChangeRequest>>().connect<&ObjectService::OnLockChange>(this);
    m_scriptAnimationConnection = aDispatcher.sink<PacketEvent<ScriptAnimationRequest>>().connect<&ObjectService::OnScriptAnimationRequest>(this);
    m_referenceEnableConnection = aDispatcher.sink<PacketEvent<RequestReferenceEnable>>().connect<&ObjectService::OnReferenceEnableRequest>(this);
    m_objectInventoryConnection = aDispatcher.sink<PacketEvent<RequestObjectInventoryChanges>>().connect<&ObjectService::OnObjectInventoryChanges>(this);
    m_trapFireConnection = aDispatcher.sink<PacketEvent<RequestTrapFire>>().connect<&ObjectService::OnTrapFire>(this);
}

// TODO(cosideci): the cell handling of objects need to be revamped.
// We already store the location and worldspace of the mod through CellIdComponent.
// Clients need a message saying the entity was destroyed.
void ObjectService::OnPlayerLeaveCellEvent(const PlayerLeaveCellEvent&) noexcept
{
    // A door state nobody can see any more is forgotten: its cell may reset, or a script may move the door, before anyone comes back.
    for (auto it = s_doorStates.begin(); it != s_doorStates.end();)
    {
        bool cSeen = false;
        for (Player* pPlayer : m_world.GetPlayerManager())
            cSeen = cSeen || it->second.Where.IsInRange(pPlayer->GetCellComponent(), false);
        it = cSeen ? std::next(it) : s_doorStates.erase(it);
    }

    // An object goes once no player has its place in range, as a door state does, not only when the cell a player left empties.
    // A game also announces the objects of the cells around its player, whose cells nobody may ever leave.
    Vector<ContainerLedger::Place> playerPlaces;
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        const CellIdComponent& cCell = pPlayer->GetCellComponent();
        playerPlaces.push_back({cCell.Cell, cCell.WorldSpaceId, cCell.CenterCoords});
    }

    auto objectView = m_world.view<FormIdComponent, ObjectComponent, CellIdComponent, InventoryComponent>();
    Vector<entt::entity> toDestroy;

    for (auto entity : objectView)
    {
        const auto& cellIdComponent = objectView.get<CellIdComponent>(entity);
        const ContainerLedger::Place cWhere{cellIdComponent.Cell, cellIdComponent.WorldSpaceId, cellIdComponent.CenterCoords};

        if (!ContainerLedger::InNobodysRange(cWhere, playerPlaces))
            continue;

        // What a container held goes to the next game in, instead of that game's own older copy.
        const auto& objectComponent = objectView.get<ObjectComponent>(entity);
        const auto& inventoryComponent = objectView.get<InventoryComponent>(entity);
        const bool cHadContent = objectComponent.HadContent || !inventoryComponent.Content.Entries.empty();
        const GameId cId = objectView.get<FormIdComponent>(entity).Id;
        if (ContainerLedger::ShouldRemember(objectComponent.IsVendor, objectComponent.SentWhole, cHadContent))
        {
            m_containerLedger.Remember(cId, inventoryComponent.Content, objectComponent.CurrentLockData, cWhere);
            spdlog::debug(
                "ContainerLedger: remembered {:X}:{:X} with {} item stack(s), {}, as no player has its cell {:X}:{:X} in range; {} kept", cId.ModId, cId.BaseId,
                inventoryComponent.Content.Entries.size(), objectComponent.CurrentLockData.IsLocked ? "locked" : "unlocked", cWhere.Cell.ModId, cWhere.Cell.BaseId,
                m_containerLedger.Size());
        }
        else
        {
            // A chest recalled earlier that is a merchant's now must not come back from an older entry.
            m_containerLedger.Forget(cId);
        }

        toDestroy.push_back(entity);
    }

    for (auto& entity : toDestroy)
    {
        m_world.destroy(entity);
    }

    if (!toDestroy.empty())
    {
        size_t remaining = 0;
        for ([[maybe_unused]] auto entity : m_world.view<ObjectComponent>())
            ++remaining;

        spdlog::debug("Objects: {} destroyed, {} object entities remain", toDestroy.size(), remaining);
    }
}

void ObjectService::OnPlayerLeave(const PlayerLeaveEvent&) noexcept
{
    // The last player out ends the session: the next one starts from the players' saves, not from what this one left.
    // The leaver is still counted here.
    if (m_world.GetPlayerManager().Count() > 1)
        return;

    spdlog::debug("ContainerLedger: forgot {} container(s), the last player left", m_containerLedger.Size());
    m_containerLedger.Clear();
}

// NOTE: this whole system kinda relies on all objects in a cell being static.
// This is fine for containers and doors, but if this system is expanded, think of temporaries.
void ObjectService::OnAssignObjectsRequest(const PacketEvent<AssignObjectsRequest>& acMessage) noexcept
{
    auto view = m_world.view<FormIdComponent, ObjectComponent, InventoryComponent>();

    AssignObjectsResponse response;

    for (const ObjectData& object : acMessage.Packet.Objects)
    {
        const auto iter = std::find_if(
            std::begin(view), std::end(view),
            [view, id = object.Id](auto entity)
            {
                const auto& formIdComponent = view.get<FormIdComponent>(entity);
                return formIdComponent.Id == id;
            });

        if (iter != std::end(view))
        {
            ObjectData objectData;
            objectData.ServerId = World::ToInteger(*iter);

            auto& formIdComponent = view.get<FormIdComponent>(*iter);
            objectData.Id = formIdComponent.Id;

            auto& objectComponent = view.get<ObjectComponent>(*iter);
            objectData.CurrentLockData = objectComponent.CurrentLockData;
            // A merchant's chest to any game stays one.
            objectComponent.IsVendor = objectComponent.IsVendor || object.IsVendor;

            auto& inventoryComponent = view.get<InventoryComponent>(*iter);
            objectData.CurrentInventory = inventoryComponent.Content;

            objectData.IsSenderFirst = false;
            if (const auto cRecord = s_doorStates.find(formIdComponent.Id); cRecord != s_doorStates.end())
                objectData.DoorOpenState = cRecord->second.State;

            response.Objects.push_back(objectData);
        }
        else
        {
            const auto cEntity = m_world.create();

            m_world.emplace<FormIdComponent>(cEntity, object.Id);

            // A container the server forgot comes back as the party left it, not as this game has it: this game takes it, as from a game already there.
            const ContainerLedger::Entry* pRemembered = object.IsVendor ? nullptr : m_containerLedger.Recall(object.Id);

            auto& objectComponent = m_world.emplace<ObjectComponent>(cEntity, acMessage.pPlayer);
            objectComponent.CurrentLockData = pRemembered ? pRemembered->Lock : object.CurrentLockData;
            objectComponent.IsVendor = object.IsVendor;
            objectComponent.HadContent = pRemembered != nullptr || !object.CurrentInventory.Entries.empty();

            m_world.emplace<CellIdComponent>(cEntity, object.CellId, object.WorldSpaceId, object.CurrentCoords);
            auto& inventoryComp = m_world.emplace<InventoryComponent>(cEntity);
            inventoryComp.Content = pRemembered ? pRemembered->Content : object.CurrentInventory;

            ObjectData objectData;
            objectData.Id = object.Id;
            objectData.ServerId = World::ToInteger(cEntity);
            objectData.IsSenderFirst = pRemembered == nullptr;
            if (pRemembered)
            {
                objectData.CurrentLockData = pRemembered->Lock;
                objectData.CurrentInventory = pRemembered->Content;
                spdlog::debug(
                    "ContainerLedger: recalled {:X}:{:X} for player {}, {} item stack(s), {}; sent in place of that game's copy", object.Id.ModId, object.Id.BaseId,
                    acMessage.pPlayer->GetId(), pRemembered->Content.Entries.size(), pRemembered->Lock.IsLocked ? "locked" : "unlocked");
            }
            if (const auto cRecord = s_doorStates.find(object.Id); cRecord != s_doorStates.end())
                objectData.DoorOpenState = cRecord->second.State;

            response.Objects.push_back(objectData);
        }
    }

    if (!acMessage.Packet.Objects.empty())
    {
        size_t entities = 0;
        for ([[maybe_unused]] auto entity : m_world.view<ObjectComponent>())
            ++entities;

        spdlog::debug("Objects: {} announced by player {}, {} object entities now", acMessage.Packet.Objects.size(), acMessage.pPlayer->GetId(), entities);
    }

    if (!response.Objects.empty())
        acMessage.pPlayer->Send(response);
}

void ObjectService::OnActivate(const PacketEvent<ActivateRequest>& acMessage) const noexcept
{
    // A door's state is kept, so a player who enters its cell later gets the door as it was left.
    if (const uint8_t cAfter = DoorState::AfterActivation(acMessage.Packet.PreActivationOpenState))
    {
        s_doorStates[acMessage.Packet.Id] = DoorRecord{cAfter, acMessage.pPlayer->GetStandingCell()};
        spdlog::debug("Doors: {:X}:{:X} left in door state {} by an activation", acMessage.Packet.Id.ModId, acMessage.Packet.Id.BaseId, cAfter);
    }

    NotifyActivate notifyActivate;
    notifyActivate.Id = acMessage.Packet.Id;
    notifyActivate.ActivatorId = acMessage.Packet.ActivatorId;
    notifyActivate.PreActivationOpenState = acMessage.Packet.PreActivationOpenState;

    // Outdoors, every player who has the door's cell loaded gets it, not only those in that very cell.
    // The sender's game made the activation, so the door is within the sender's loaded grid.
    const CellIdComponent cSenderCell = acMessage.pPlayer->GetStandingCell();
    for (auto pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer == acMessage.pPlayer)
            continue;

        const CellIdComponent& cCell = pPlayer->GetCellComponent();
        const bool cSameCell = cCell.Cell == acMessage.Packet.CellId;
        const bool cLoadedThere = !cSenderCell.IsInInteriorCell() && !cCell.IsInInteriorCell() && cSenderCell.IsInRange(cCell, false);
        if (cSameCell || cLoadedThere)
            pPlayer->Send(notifyActivate);
    }
}

void ObjectService::OnTrapFire(const PacketEvent<RequestTrapFire>& acMessage) noexcept
{
    const auto& cMessage = acMessage.Packet;

    // Only from a player near the trap: indoors in its very cell, outdoors within the grid their game has loaded.
    const CellIdComponent cTrapCell{cMessage.CellId, cMessage.WorldSpaceId, cMessage.CenterCoords};
    if (!acMessage.pPlayer->GetStandingCell().IsInRange(cTrapCell, false))
        return;

    // A count lasts a second, so the players' counts are dropped once there are many.
    if (m_trapFireRates.size() > 64)
        m_trapFireRates.clear();
    if (!m_trapFireRates[acMessage.pPlayer->GetId()].Allow(std::chrono::steady_clock::now()))
        return;

    NotifyTrapFire notify;
    notify.Id = cMessage.Id;
    notify.TriggerId = cMessage.TriggerId;

    // To those who have the trap loaded.
    for (auto pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer != acMessage.pPlayer && pPlayer->GetCellComponent().IsInRange(cTrapCell, false))
            pPlayer->Send(notify);
    }
}

void ObjectService::OnLockChange(const PacketEvent<LockChangeRequest>& acMessage) const noexcept
{
    NotifyLockChange notifyLockChange;
    notifyLockChange.Id = acMessage.Packet.Id;
    notifyLockChange.IsLocked = acMessage.Packet.IsLocked;
    notifyLockChange.LockLevel = acMessage.Packet.LockLevel;

    auto objectView = m_world.view<FormIdComponent, ObjectComponent>();

    const auto iter = std::find_if(
        std::begin(objectView), std::end(objectView),
        [objectView, id = acMessage.Packet.Id](auto entity)
        {
            const auto& formIdComponent = objectView.get<FormIdComponent>(entity);
            return formIdComponent.Id == id;
        });

    if (iter != std::end(objectView))
    {
        auto& objectComponent = objectView.get<ObjectComponent>(*iter);
        objectComponent.CurrentLockData.IsLocked = acMessage.Packet.IsLocked;
        objectComponent.CurrentLockData.LockLevel = acMessage.Packet.LockLevel;
    }

    // A locked door is no longer open as an activation left it: a later arrival must not open it.
    if (acMessage.Packet.IsLocked)
        s_doorStates.erase(acMessage.Packet.Id);

    // Outdoors, every player who has the reference's cell loaded gets it, not only those in that very cell (as OnActivate).
    // The sender's game changed the lock, so the reference is within the sender's loaded grid.
    const CellIdComponent cSenderCell = acMessage.pPlayer->GetStandingCell();
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer == acMessage.pPlayer)
            continue;

        const CellIdComponent& cCell = pPlayer->GetCellComponent();
        const bool cSameCell = cCell.Cell == acMessage.Packet.CellId;
        const bool cLoadedThere = !cSenderCell.IsInInteriorCell() && !cCell.IsInInteriorCell() && cSenderCell.IsInRange(cCell, false);
        if (cSameCell || cLoadedThere)
            pPlayer->Send(notifyLockChange);
    }
}

void ObjectService::OnScriptAnimationRequest(const PacketEvent<ScriptAnimationRequest>& acMessage) noexcept
{
    auto& packet = acMessage.Packet;

    NotifyScriptAnimation message{};
    message.FormID = packet.FormID;
    message.Animation = packet.Animation;
    message.EventName = packet.EventName;

    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        pPlayer->Send(message);
    }
}

void ObjectService::OnReferenceEnableRequest(const PacketEvent<RequestReferenceEnable>& acMessage) noexcept
{
    // The party leader's game runs the quests whose scripts switch the world's references (a siege's camps and barricades).
    // Any member's lever or button switches them too: taken from a member who used a mechanism of their cell just now (SharedHomes).
    auto& partyService = m_world.GetPartyService();
    PartyService::Party* pParty = partyService.GetPlayerParty(acMessage.pPlayer);
    const bool cLeader = pParty && partyService.IsPlayerLeader(acMessage.pPlayer);
    // Or that member's own quest just reached a step new to the party (QuestService::SteppedRecently).
    const bool cUsedRecently =
        pParty && !cLeader && (m_world.ctx().at<ActivationService>().UsedRecently(acMessage.pPlayer) || m_world.GetQuestService().SteppedRecently(acMessage.pPlayer));
    const size_t cTaken = SharedHomes::SwitchesTaken(pParty != nullptr, cLeader, cUsedRecently, acMessage.Packet.Changes.size());
    if (cTaken == 0)
    {
        if (pParty && !cLeader)
            spdlog::debug(
                "ReferenceEnable: {} switch(es) from member {} not taken, no recent use of a lever or button nor a new quest step", acMessage.Packet.Changes.size(),
                acMessage.pPlayer->GetId());
        return;
    }

    NotifyReferenceEnable notify{};
    if (cLeader)
        notify.Changes = acMessage.Packet.Changes;
    else
    {
        if (m_memberSwitchRates.size() > 64)
            m_memberSwitchRates.clear();
        if (!m_memberSwitchRates[acMessage.pPlayer->GetId()].Allow(std::chrono::steady_clock::now()))
        {
            spdlog::debug("ReferenceEnable: switches from member {} not taken, too many messages", acMessage.pPlayer->GetId());
            return;
        }

        // A member switches places' references, never a character: those stay the leader's (client ObjectService::SwitchActor).
        TiltedPhoques::Set<GameId> characters;
        const auto characterView = m_world.view<FormIdComponent, CharacterComponent>();
        for (const auto cEntity : characterView)
            characters.insert(characterView.get<FormIdComponent>(cEntity).Id);
        for (size_t i = 0; i < cTaken; ++i)
            if (!characters.contains(acMessage.Packet.Changes[i].Id))
                notify.Changes.push_back(acMessage.Packet.Changes[i]);
        if (notify.Changes.size() < acMessage.Packet.Changes.size())
            spdlog::debug("ReferenceEnable: {} of {} switch(es) from member {} taken", notify.Changes.size(), acMessage.Packet.Changes.size(), acMessage.pPlayer->GetId());
        if (notify.Changes.empty())
            return;
    }

    // Kept per party in the order they came, the last switch of each reference winning, so a member who joins later gets them too
    // (PartyService::SendPartyJoinedEvent).
    for (const auto& cChange : notify.Changes)
        pParty->ReferenceEnabled.Set(cChange.Id, cChange.Enabled);
    for (Player* pMember : pParty->Members)
    {
        if (pMember != acMessage.pPlayer)
            pMember->Send(notify);
    }

    // A character switched off leaves the server: a spawn sent to a game coming into range would switch it back on there.
    TiltedPhoques::Vector<uint32_t> switchedOff;
    const auto characters = m_world.view<FormIdComponent, CharacterComponent>();
    for (const auto cEntity : characters)
    {
        const auto cEnabled = pParty->ReferenceEnabled.Find(characters.get<FormIdComponent>(cEntity).Id);
        if (cEnabled && !*cEnabled)
            switchedOff.push_back(World::ToInteger(cEntity));
    }
    for (const uint32_t cServerId : switchedOff)
        m_world.GetDispatcher().trigger(CharacterRemoveEvent(cServerId));

    // At most one line every 10 s, with how many came since the last.
    static std::chrono::steady_clock::time_point s_nextLog{};
    static size_t s_since = 0;
    s_since += acMessage.Packet.Changes.size();
    if (const auto cNow = std::chrono::steady_clock::now(); cNow >= s_nextLog)
    {
        s_nextLog = cNow + std::chrono::seconds(10);
        spdlog::info(
            "ReferenceEnable: {} reference(s) switched by player {}, relayed to {} member(s); the party keeps {}", s_since, acMessage.pPlayer->GetId(), pParty->Members.size() - 1,
            pParty->ReferenceEnabled.Size());
        s_since = 0;
    }
}

// A whole container from one game, sent when that game's own engine changed it past the item hooks: a merchant's restock as its barter menu opens.
// It replaces what the server keeps, and goes to the other players who have that container's cell in range.
void ObjectService::OnObjectInventoryChanges(const PacketEvent<RequestObjectInventoryChanges>& acMessage) noexcept
{
    auto view = m_world.view<FormIdComponent, ObjectComponent, InventoryComponent, CellIdComponent>();

    for (const auto& [cId, cObject] : acMessage.Packet.Changes)
    {
        const auto cIter = std::find_if(std::begin(view), std::end(view), [view, id = cId](auto entity) { return view.get<FormIdComponent>(entity).Id == id; });
        if (cIter == std::end(view))
            continue;

        // Only from a game that has the container in range, as for any change to it.
        if (!acMessage.pPlayer->GetCellComponent().IsInRange(view.get<CellIdComponent>(*cIter), false))
        {
            spdlog::debug("Object inventory: {:X}:{:X} from player {} not taken, the container is out of their range", cId.ModId, cId.BaseId, acMessage.pPlayer->GetId());
            continue;
        }

        // Only a merchant's chest is sent whole. A game announces it as one before it trades (IsVendor); the server cannot tell from the form alone.
        auto& objectComponent = view.get<ObjectComponent>(*cIter);
        if (!objectComponent.IsVendor)
        {
            spdlog::debug("Object inventory: {:X}:{:X} from player {} not taken, it is not a merchant's chest", cId.ModId, cId.BaseId, acMessage.pPlayer->GetId());
            continue;
        }

        view.get<InventoryComponent>(*cIter).Content = cObject.CurrentInventory;
        // The game restocks it: the ContainerLedger must not keep it.
        objectComponent.SentWhole = true;

        NotifyObjectInventoryChanges notify{};
        notify.Changes[cId] = cObject.CurrentInventory;
        if (!GameServer::Get()->SendToPlayersInRange(notify, *cIter, acMessage.GetSender()))
            spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
    }
}
