#include <Services/GameExtension.h>
#include <Services/ObjectService.h>
#include <SharedHomes.h>
#include <Services/QuestService.h>
#include <Helpers/PerfScope.h>

#include <World.h>
#include <Components.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/CellChangeEvent.h>
#include <Events/ActivateEvent.h>
#include <Events/LockChangeEvent.h>
#include <Events/ScriptAnimationEvent.h>
#include <Messages/ServerTimeSettings.h>
#include <Messages/AssignObjectsRequest.h>
#include <Messages/AssignObjectsResponse.h>
#include <Messages/ActivateRequest.h>
#include <Messages/NotifyActivate.h>
#include <Messages/LockChangeRequest.h>
#include <Messages/NotifyLockChange.h>
#include <Messages/ScriptAnimationRequest.h>
#include <Messages/NotifyScriptAnimation.h>
#include <Messages/RequestReferenceEnable.h>
#include <Messages/NotifyReferenceEnable.h>
#include <Messages/RequestObjectInventoryChanges.h>
#include <Messages/NotifyObjectInventoryChanges.h>
#include <Messages/RequestTrapFire.h>
#include <Messages/NotifyTrapFire.h>
#include <Events/TrapFireEvent.h>
#include <Services/ActivationCatchUpService.h>
#include <ReferenceStates.h>
#include <Events/ReferenceEnableEvent.h>
#include <Events/ScriptDoorEvent.h>
#include <Services/PartyService.h>
#include <Structs/DoorState.h>
#include <Services/Generic/ContainerLidRules.h>
#include <glm/geometric.hpp>

#include <PlayerCharacter.h>
#include <Games/ActorExtension.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/BGSEncounterZone.h>
#include <Forms/TESFaction.h>
#include <Forms/TESNPC.h>
#include <ExtraData/ExtraFactionChanges.h>
#include <Games/Misc/MenuTopicManager.h>
#include <Games/TES.h>

#include <inttypes.h>
#include <unordered_set>

// The states travel as the game's own numbers.
static_assert(DoorState::kOpen == TESObjectREFR::kOpen && DoorState::kOpening == TESObjectREFR::kOpening && DoorState::kClosed == TESObjectREFR::kClosed &&
              DoorState::kClosing == TESObjectREFR::kClosing && DoorState::kNone == TESObjectREFR::kNone);

ObjectService::ObjectService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&ObjectService::OnDisconnected>(this);
    m_cellChangeConnection = aDispatcher.sink<CellChangeEvent>().connect<&ObjectService::OnCellChange>(this);
    m_onActivateConnection = aDispatcher.sink<ActivateEvent>().connect<&ObjectService::OnActivate>(this);
    m_activateConnection = aDispatcher.sink<NotifyActivate>().connect<&ObjectService::OnActivateNotify>(this);
    m_lockChangeConnection = aDispatcher.sink<LockChangeEvent>().connect<&ObjectService::OnLockChange>(this);
    m_lockChangeNotifyConnection = aDispatcher.sink<NotifyLockChange>().connect<&ObjectService::OnLockChangeNotify>(this);
    m_assignObjectConnection = aDispatcher.sink<AssignObjectsResponse>().connect<&ObjectService::OnAssignObjectsResponse>(this);
    m_scriptAnimationConnection = aDispatcher.sink<ScriptAnimationEvent>().connect<&ObjectService::OnScriptAnimationEvent>(this);
    m_scriptAnimationNotifyConnection = aDispatcher.sink<NotifyScriptAnimation>().connect<&ObjectService::OnNotifyScriptAnimation>(this);
    m_referenceEnableConnection = aDispatcher.sink<ReferenceEnableEvent>().connect<&ObjectService::OnReferenceEnableEvent>(this);
    m_referenceEnableNotifyConnection = aDispatcher.sink<NotifyReferenceEnable>().connect<&ObjectService::OnNotifyReferenceEnable>(this);
    m_scriptDoorConnection = aDispatcher.sink<ScriptDoorEvent>().connect<&ObjectService::OnScriptDoorEvent>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&ObjectService::OnUpdate>(this);
    m_objectInventoryConnection = aDispatcher.sink<NotifyObjectInventoryChanges>().connect<&ObjectService::OnNotifyObjectInventoryChanges>(this);
    m_trapFireConnection = aDispatcher.sink<TrapFireEvent>().connect<&ObjectService::OnTrapFire>(this);
    m_trapFireNotifyConnection = aDispatcher.sink<NotifyTrapFire>().connect<&ObjectService::OnTrapFireNotify>(this);

    EventDispatcherManager::Get()->activateEvent.RegisterSink(this);
    EventDispatcherManager::Get()->triggerEnterEvent.RegisterSink(this);
    EventDispatcherManager::Get()->triggerLeaveEvent.RegisterSink(this);
}

bool ObjectService::IsPlayerHome(const TESObjectCELL* apCell) noexcept
{
    if (apCell && apCell->loadedCellData && apCell->loadedCellData->encounterZone)
    {
        // Only return true if cell has the NoResetZone encounter zone
        if (apCell->loadedCellData->encounterZone->formID == SharedHomes::kNoResetZone)
        {
            switch (apCell->formID)
            {
            case 0xeec55: // one known exception: Sinderion's Field Lab
                return false;
            default: return true;
            }
        }
    }

    return false;
}

// Find each loaded faction's containers for a jailed player's belongings and stolen items.
// Do not sync them: each player's items must stay separate (#700).
// Only compare the container pointers; never read through them.
Set<const TESObjectREFR*> GetPlayerStashContainers() noexcept
{
    Set<const TESObjectREFR*> containers{};

    ModManager* pModManager = ModManager::Get();
    if (!pModManager)
        return containers;

    for (const TESFaction* pFaction : pModManager->factions)
    {
        if (!pFaction)
            continue;

        if (pFaction->crimeData.playerInventoryContainer)
            containers.insert(pFaction->crimeData.playerInventoryContainer);

        if (pFaction->crimeData.stolenGoodsContainer)
            containers.insert(pFaction->crimeData.stolenGoodsContainer);
    }

    return containers;
}

// Find each loaded faction's merchant chest, where its vendors keep their wares and gold.
// The game restocks them, so the server's ContainerLedger must not keep them.
// Only compare the container pointers; never read through them.
Set<const TESObjectREFR*> GetMerchantContainers() noexcept
{
    Set<const TESObjectREFR*> containers{};

    ModManager* pModManager = ModManager::Get();
    if (!pModManager)
        return containers;

    for (const TESFaction* pFaction : pModManager->factions)
    {
        if (pFaction && pFaction->vendorData.merchantContainer)
            containers.insert(pFaction->vendorData.merchantContainer);
    }

    return containers;
}

bool ShouldSyncObject(const TESObjectREFR* apObject, const Set<const TESObjectREFR*>& acPlayerStashContainers) noexcept
{
    if (!apObject)
        return false;

    if (acPlayerStashContainers.contains(apObject))
        return false;

    // Quest chests that take the player's whole inventory without going through faction crime data.
    switch (apObject->formID)
    {
    case 0x39CF1: // Don't sync the chest in the "Diplomatic Immunity" quest
        return false;
    case 0x3EF03: // ...as well as in the "No One Escapes Cidhna Mine" quest
        return false;
    default:
        return true;
    }
}

namespace
{
// The chest of the merchant this player is speaking to, 0 when none.
uint32_t SpokenMerchantChest() noexcept
{
    const MenuTopicManager* pManager = MenuTopicManager::Get();
    if (!pManager || !pManager->menuOpen || !pManager->speaker)
        return 0;

    const Actor* pSpeaker = Cast<Actor>(TESObjectREFR::GetByHandle(pManager->speaker.handle.iBits));
    if (!pSpeaker)
        return 0;

    // A faction in the actor's changes replaces the one in its record, a rank below zero taking it away.
    std::vector<VendorChest::Membership> memberships;
    Set<const TESFaction*> changed;
    const auto addMembership = [&memberships](const TESFaction* apFaction, int8_t aRank)
    {
        const TESObjectREFR* pChest = apFaction->vendorData.merchantContainer;
        memberships.push_back({apFaction->IsVendor(), aRank, pChest ? pChest->formID : 0});
    };

    if (const auto* pChanges = Cast<ExtraFactionChanges>(pSpeaker->extraData.GetByType(ExtraDataType::Faction)))
    {
        for (auto i = 0u; i < pChanges->entries.length; ++i)
        {
            if (const TESFaction* pFaction = pChanges->entries[i].faction)
            {
                changed.insert(pFaction);
                addMembership(pFaction, pChanges->entries[i].rank);
            }
        }
    }

    if (const auto* pNpc = Cast<TESNPC>(pSpeaker->baseForm))
    {
        const auto& cFactions = pNpc->actorData.factions;
        for (auto i = 0u; i < cFactions.length; ++i)
        {
            if (const TESFaction* pFaction = cFactions[i].faction; pFaction && !changed.contains(pFaction))
                addMembership(pFaction, cFactions[i].rank);
        }
    }

    return VendorChest::PickChest(memberships);
}

bool IsBarterMenuOpen() noexcept
{
    static const BSFixedString s_name("BarterMenu");
    const UI* pUI = UI::Get();
    return pUI && pUI->GetMenuOpen(s_name);
}
} // namespace

void ObjectService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_enableChanges.clear();
    m_enableWaiting.clear();
    m_appliedSwitches.clear();
    m_ownUseAt = {};
    m_replayAt = {};
    m_ownStepAt.store(0, std::memory_order_relaxed);
    m_questReplayAt.store(0, std::memory_order_relaxed);
    m_doorsMovedHere.clear();
    m_vendorTrade = {};
    m_vendorChestAnswered = 0;
    m_announcedObjects.Clear();
    ForgetObjectsLeft();
    m_traps.Clear();
    TrapSync::SharedLedger().Clear();
}

void ObjectService::ForgetObjectsLeft() noexcept
{
    // The chest of the trade going on stays: it is shared from wherever its merchant keeps it.
    const uint32_t cTradeChest = m_vendorTrade.ChestId();
    const auto view = m_world.view<FormIdComponent, ObjectComponent>();
    Vector<entt::entity> left;
    for (const auto cEntity : view)
    {
        const uint32_t cFormId = view.get<FormIdComponent>(cEntity).Id;
        if (cFormId != cTradeChest && !m_announcedObjects.Contains(cFormId))
            left.push_back(cEntity);
    }

    if (left.empty())
        return;

    m_world.destroy(left.begin(), left.end());
    spdlog::debug("Objects: {} object entities dropped, their cells no longer around the player", left.size());
}

void ObjectService::OnCellChange(const CellChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    TESObjectCELL* pCell = pPlayer->parentCell;

    // A player home is a shared house with each player's own storage (SharedHomes): its doors are announced, its chests are not,
    // so chests used as storage are never wiped by another player's game.
    const bool cSyncHomes = World::Get().GetServerSettings().SyncPlayerHomes;

    GameId worldSpaceId{};
    TESWorldSpace* pWorldSpace = pPlayer->GetWorldSpace();
    if (pWorldSpace)
    {
        if (!m_world.GetModSystem().GetServerModId(pWorldSpace->formID, worldSpaceId))
        {
            spdlog::error("Server world space id not found for world space form id {:X}", pWorldSpace->formID);
            m_announcedObjects.Clear();
            ForgetObjectsLeft();
            return;
        }
    }

    // Indoors the player's own cell, as always. Outdoors the cells around it too, each one the game has attached,
    // so a change to a chest in the next cell over reaches the other games.
    Vector<TESObjectCELL*> cells;
    cells.push_back(pCell);
    size_t cellsNotAttached = 0;
    if (pWorldSpace)
    {
        const TES* pTES = TES::Get();
        for (const ChestSelection::Cell& cGrid : ChestSelection::CellsAround(pTES->currentGridX, pTES->currentGridY, ChestSelection::cCellsOut))
        {
            TESObjectCELL* pAround = ModManager::Get()->GetCellFromCoordinates(cGrid.X, cGrid.Y, pWorldSpace, false);
            if (!pAround || pAround == pCell)
                continue;

            if (!pAround->IsAttached())
            {
                ++cellsNotAttached;
                continue;
            }

            cells.push_back(pAround);
        }
    }

    struct Candidate
    {
        TESObjectREFR* pObject;
        GameId Id;
        GameId CellId;
    };

    const Vector<FormType> cFormTypes = {FormType::Container, FormType::Door};
    const Set<const TESObjectREFR*> playerStashContainers = GetPlayerStashContainers();
    Vector<Candidate> candidates;
    std::vector<uint32_t> candidateIds;

    for (TESObjectCELL* pObjectsCell : cells)
    {
        if (!pObjectsCell)
            continue;

        GameId cellId{};
        if (!m_world.GetModSystem().GetServerModId(pObjectsCell->formID, cellId))
        {
            spdlog::error("Server cell id not found for cell form id {:X}", pObjectsCell->formID);
            continue;
        }

        const bool cHome = IsPlayerHome(pObjectsCell);

        // Door seemed to be at the wrong form id (29, now 32), verify this.
        for (TESObjectREFR* pObject : pObjectsCell->GetRefsByFormTypes(cFormTypes))
        {
            if (!SharedHomes::IsShared(cHome, pObject->baseForm && pObject->baseForm->formType == FormType::Container, cSyncHomes))
                continue;

            if (!ShouldSyncObject(pObject, playerStashContainers))
            {
                spdlog::warn("Excluding sync for {:X}", pObject->formID);
                continue;
            }

            Candidate candidate{pObject, {}, cellId};
            if (!m_world.GetModSystem().GetServerModId(pObject->formID, candidate.Id))
            {
                spdlog::error("Server form id not found for object with form id {:X}", pObject->formID);
                continue;
            }

            // Each object goes with its own cell, not the player's: the server keeps it while a player has that cell in range.
            const TESObjectCELL* pOwnCell = pObject->parentCell;
            if (pOwnCell && pOwnCell != pObjectsCell && !m_world.GetModSystem().GetServerModId(pOwnCell->formID, candidate.CellId))
            {
                spdlog::error("Server cell id not found for cell form id {:X} of object {:X}", pOwnCell->formID, pObject->formID);
                continue;
            }

            candidates.push_back(candidate);
            candidateIds.push_back(pObject->formID);
        }
    }

    // Only what was not announced since it came into these cells: the server still has the rest.
    const std::vector<size_t> toAnnounce = m_announcedObjects.Update(candidateIds);
    ForgetObjectsLeft();
    if (toAnnounce.empty())
        return;

    const Set<const TESObjectREFR*> merchantContainers = GetMerchantContainers();
    Vector<ObjectData> objects;

    for (const size_t cIndex : toAnnounce)
    {
        TESObjectREFR* pObject = candidates[cIndex].pObject;

        ObjectData objectData{};
        objectData.Id = candidates[cIndex].Id;
        objectData.CellId = candidates[cIndex].CellId;
        objectData.WorldSpaceId = worldSpaceId;
        objectData.CurrentCoords = GridCellCoords::CalculateGridCellCoords(pObject->position.x, pObject->position.y);

        if (Lock* pLock = pObject->GetLock())
        {
            objectData.CurrentLockData.IsLocked = pLock->IsLocked();
            objectData.CurrentLockData.LockLevel = pLock->lockLevel;
        }

        if (pObject->baseForm->formType == FormType::Container)
        {
            objectData.CurrentInventory = pObject->GetInventory();
            objectData.IsVendor = merchantContainers.contains(pObject);
        }

        objects.push_back(objectData);
    }

    // A request counts its objects in 8 bits, and a town's first announcement must not make one huge packet.
    const auto cBatches = ChestSelection::Batches(objects.size(), ChestSelection::cMaxPerRequest);
    for (const auto& cBatch : cBatches)
    {
        AssignObjectsRequest request{};
        for (size_t i = cBatch.first; i < cBatch.second; ++i)
            request.Objects.push_back(objects[i]);

        m_transport.Send(request);
    }

    spdlog::debug(
        "Objects: {} container(s) and door(s) announced from {} cell(s) around the player ({} not attached, skipped) in {} request(s); {} announced in all", objects.size(),
        cells.size(), cellsNotAttached, cBatches.size(), m_announcedObjects.Size());
}

void ObjectService::OnAssignObjectsResponse(const AssignObjectsResponse& acMessage) noexcept
{
    for (const ObjectData& objectData : acMessage.Objects)
    {
        const uint32_t cObjectId = World::Get().GetModSystem().GetGameId(objectData.Id);
        TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
        if (!pObject)
        {
            spdlog::error("Object not found for form id {:X} (server mod {:X}, base {:X})", cObjectId, objectData.Id.ModId, objectData.Id.BaseId);
            continue;
        }

        CreateObjectEntity(pObject->formID, objectData.ServerId);
        if (pObject->formID == m_vendorTrade.ChestId())
            m_vendorChestAnswered = pObject->formID;

        if (!objectData.IsSenderFirst && objectData.CurrentLockData != LockData{})
        {
            Lock* pLock = pObject->GetLock();
            if (!pLock)
                pLock = pObject->CreateLock();
            if (pLock)
            {
                pLock->lockLevel = objectData.CurrentLockData.LockLevel;
                pLock->SetLock(objectData.CurrentLockData.IsLocked);
                pObject->LockChange();
            }
        }

        // A door used in another game while this one was away is set to the state the server last passed on.
        // It comes even when this game is the first in the cell: the door may have been used from the next cell over.
        // Setting it is not an activation, so nothing is sent back.
        const auto cShared = static_cast<TESObjectREFR::OpenState>(objectData.DoorOpenState);
        if (cShared != TESObjectREFR::kNone && pObject->baseForm->formType == FormType::Door && !pObject->extraData.Contains(ExtraDataType::Teleport))
        {
            const bool cOpen = cShared == TESObjectREFR::kOpen;
            const TESObjectREFR::OpenState cLocal = pObject->GetOpenState();
            const Lock* pLock = pObject->GetLock();
            // A door locked here stays shut: opening it would unlock it, as the game's SetOpen does.
            const bool cLockedHere = pLock && pLock->IsLocked();
            const bool cMovedHere = m_doorsMovedHere.contains(pObject->formID);
            if (cLocal == TESObjectREFR::kNone || cLocal == cShared || cLocal == (cOpen ? TESObjectREFR::kOpening : TESObjectREFR::kClosing) || (cOpen && cLockedHere) || cMovedHere)
            {
                spdlog::info("Door {:X}: {} in the other games when this game came in; {} here (state {})", pObject->formID, cOpen ? "open" : "closed",
                             cLocal == TESObjectREFR::kNone ? "no state known" : cOpen && cLockedHere ? "left shut, it is locked" : cMovedHere ? "left as this game's script left it" : "already so",
                             static_cast<int>(cLocal));
            }
            else
            {
                pObject->SetOpen(cOpen);
                spdlog::info("Door {:X}: {} in the other games when this game came in; set {} here from state {}", pObject->formID, cOpen ? "open" : "closed",
                             cOpen ? "open" : "closed", static_cast<int>(cLocal));
            }
        }

        if (objectData.IsSenderFirst)
            continue;

        if (pObject->baseForm->formType == FormType::Container)
        {
            if (m_vendorTrade.Defer(pObject->formID))
            {
                spdlog::debug("VendorChest: the other games' {:X} arrived during this player's barter; this game's goes to them when the menu closes", pObject->formID);
                continue;
            }

            Inventory currentInventory = pObject->GetInventory();

            if (currentInventory.ContainsQuestItems())
                pObject->SetInventoryRetainingQuestItems(currentInventory, objectData.CurrentInventory);
            else
                pObject->SetInventory(objectData.CurrentInventory);

            if (pObject->formID == m_vendorTrade.ChestId())
                m_vendorChestSeen = pObject->GetInventory();
        }
    }
}

bool ObjectService::IsObjectShared(uint32_t aFormId) const noexcept
{
    const auto view = m_world.view<FormIdComponent, ObjectComponent>();
    return std::any_of(view.begin(), view.end(), [view, aFormId](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == aFormId; });
}

void ObjectService::RunVendorChest() noexcept
{
    if (!m_transport.IsConnected())
        return;

    // While the barter menu is open the trade goes on with the same merchant, whatever the dialogue says.
    const bool cBarterMenuOpen = IsBarterMenuOpen();
    uint32_t chestId = SpokenMerchantChest();
    if (!chestId && cBarterMenuOpen)
        chestId = m_vendorTrade.ChestId();
    const bool cBarterOpen = chestId && cBarterMenuOpen;

    // Shared anew at each trade: the server may have forgotten it since, as the cell emptied.
    if (chestId != m_vendorTrade.ChestId())
        m_vendorChestAnswered = 0;

    const auto cRead = [](uint32_t aChestId)
    {
        const auto* pChest = Cast<TESObjectREFR>(TESForm::GetById(aChestId));
        return pChest ? pChest->GetInventory() : Inventory{};
    };
    const VendorChest::Trade::Action cAction = VendorChest::RunFrame(m_vendorTrade, m_vendorChestSeen, cRead, chestId, cBarterOpen, chestId && m_vendorChestAnswered == chestId);
    switch (cAction.What)
    {
    case VendorChest::Trade::Step::kRegister: ShareVendorChest(cAction.ChestId); break;
    case VendorChest::Trade::Step::kCheckRestock:
        // The game restocks a merchant's chest as its barter menu opens, which no item hook sees.
        spdlog::debug("VendorChest: {:X} changed here as its barter menu opened (a restock); sent whole to the other games", cAction.ChestId);
        SendWholeChest(cAction.ChestId);
        break;
    case VendorChest::Trade::Step::kSendAll:
        spdlog::debug("VendorChest: {:X} sent whole to the other games after this player's trade, which overlapped another game's", cAction.ChestId);
        SendWholeChest(cAction.ChestId);
        break;
    default: break;
    }
}

// The chest is shared as a container of the player's own cell: the server passes its changes to every player there and forgets it once no player has that cell in range.
void ObjectService::ShareVendorChest(uint32_t aChestId) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    TESObjectREFR* pChest = Cast<TESObjectREFR>(TESForm::GetById(aChestId));
    if (!pPlayer || !pPlayer->parentCell || !pChest || !pChest->baseForm || pChest->baseForm->formType != FormType::Container)
        return;

    if (!World::Get().GetServerSettings().SyncPlayerHomes && IsPlayerHome(pPlayer->parentCell))
        return;

    if (!ShouldSyncObject(pChest, GetPlayerStashContainers()))
        return;

    ObjectData objectData{};
    auto& modSystem = m_world.GetModSystem();
    if (!modSystem.GetServerModId(pPlayer->parentCell->formID, objectData.CellId) || !modSystem.GetServerModId(aChestId, objectData.Id))
    {
        spdlog::debug("VendorChest: {:X} not shared, it or this cell has no server id", aChestId);
        return;
    }

    if (TESWorldSpace* pWorldSpace = pPlayer->GetWorldSpace(); pWorldSpace && !modSystem.GetServerModId(pWorldSpace->formID, objectData.WorldSpaceId))
        return;

    objectData.CurrentCoords = GridCellCoords::CalculateGridCellCoords(pPlayer->position.x, pPlayer->position.y);
    objectData.CurrentInventory = pChest->GetInventory();
    // The game restocks it, so the server's ContainerLedger must not keep it.
    objectData.IsVendor = true;
    m_vendorChestSeen = objectData.CurrentInventory;

    AssignObjectsRequest request{};
    request.Objects.push_back(objectData);
    m_transport.Send(request);

    spdlog::debug("VendorChest: merchant chest {:X} shared while this player trades", aChestId);
}

void ObjectService::SendWholeChest(uint32_t aChestId) noexcept
{
    const auto* pChest = Cast<TESObjectREFR>(TESForm::GetById(aChestId));
    ObjectData objectData{};
    if (!pChest || !m_world.GetModSystem().GetServerModId(aChestId, objectData.Id))
        return;

    objectData.CurrentInventory = pChest->GetInventory();
    m_vendorChestSeen = objectData.CurrentInventory;

    RequestObjectInventoryChanges request{};
    request.Changes[objectData.Id] = objectData;
    m_transport.Send(request);
}

void ObjectService::OnNotifyObjectInventoryChanges(const NotifyObjectInventoryChanges& acMessage) noexcept
{
    for (const auto& [cId, cInventory] : acMessage.Changes)
    {
        const uint32_t cFormId = m_world.GetModSystem().GetGameId(cId);
        TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(cFormId));
        if (!pObject || !pObject->baseForm || pObject->baseForm->formType != FormType::Container || !IsObjectShared(cFormId))
            continue;

        if (m_vendorTrade.Defer(cFormId))
        {
            spdlog::debug("VendorChest: another game's whole {:X} arrived during this player's barter; this game's goes to them when the menu closes", cFormId);
            continue;
        }

        Inventory currentInventory = pObject->GetInventory();
        if (currentInventory.ContainsQuestItems())
            pObject->SetInventoryRetainingQuestItems(currentInventory, cInventory);
        else
            pObject->SetInventory(cInventory);

        if (cFormId == m_vendorTrade.ChestId())
            m_vendorChestSeen = pObject->GetInventory();

        spdlog::debug("VendorChest: {:X} set as another game sent it whole", cFormId);
    }
}

entt::entity ObjectService::CreateObjectEntity(const uint32_t acFormId, const uint32_t acServerId) noexcept
{
    const auto view = m_world.view<FormIdComponent, ObjectComponent>();

    if (const auto existing = Utils::FindById<ObjectComponent>(view, acServerId))
        return *existing;

    // The server gives an object a new id when it is announced again; an answer can come for one whose entity is still here.
    // The same reference keeps one entity, so its changes go to the id the server has now.
    if (const auto existing = Utils::FindByFormId(view, acFormId))
    {
        m_world.patch<ObjectComponent>(*existing, [acServerId](ObjectComponent& aObject) { aObject.Id = acServerId; });
        return *existing;
    }

    entt::entity entity = m_world.create();
    spdlog::debug("Created object entity, server id: {:X}, form id {:X}", acServerId, acFormId);

    m_world.emplace<FormIdComponent>(entity, acFormId);
    m_world.emplace<ObjectComponent>(entity, acServerId);

    return entity;
}

namespace
{
// Chests whose lid this game opened for another player's use, by form id (ContainerLidRules).
struct OpenLid
{
    uint32_t UserFormId{};
    std::chrono::steady_clock::time_point OpenedAt{};
};
TiltedPhoques::Map<uint32_t, OpenLid> s_openLids;

/**
 * Send a chest's lid animation, opening or closing, by the chest graph's own event.
 * A chest with no animated lid has no such event, and nothing happens.
 *
 * @param apChest The chest.
 * @param aOpen True to open it, false to close it.
 * @return True when the chest's graph took the event.
 */
bool AnimateLid(TESObjectREFR* apChest, const bool aOpen) noexcept
{
    static BSFixedString s_open("Open");
    static BSFixedString s_close("Close");
    return apChest->SendAnimationEvent(aOpen ? &s_open : &s_close);
}

/**
 * Open a chest's lid here because another player opened that chest in their game: the use replayed here opens no menu and no lid.
 *
 * @param apChest The chest.
 * @param apUser That player's character here.
 */
void OpenLidFor(TESObjectREFR* apChest, const Actor* apUser) noexcept
{
    // Sent again on each use: the lid may have closed meanwhile (this player opened and closed that chest), and an open lid ignores it.
    if (!AnimateLid(apChest, true) && !s_openLids.contains(apChest->formID))
    {
        spdlog::debug("ContainerLid: {:X} opened by {:X} in their game; its graph took no Open event here (no lid)", apChest->formID, apUser->formID);
        return;
    }
    s_openLids[apChest->formID] = OpenLid{apUser->formID, std::chrono::steady_clock::now()};
    spdlog::debug("ContainerLid: {:X} opened by {:X} in their game; lid opened here", apChest->formID, apUser->formID);
}

/**
 * Close the lids opened for other players once their characters left the chests (ContainerLidRules::ShouldClose).
 * All are closed when not connected.
 *
 * @param aConnected Whether this game is connected.
 */
void RunContainerLids(const bool aConnected) noexcept
{
    if (s_openLids.empty())
        return;
    static std::chrono::steady_clock::time_point s_nextRun{};
    const auto cNow = std::chrono::steady_clock::now();
    if (aConnected && cNow < s_nextRun)
        return;
    s_nextRun = cNow + std::chrono::milliseconds(250);

    for (auto it = s_openLids.begin(); it != s_openLids.end();)
    {
        TESObjectREFR* pChest = Cast<TESObjectREFR>(TESForm::GetById(it->first));
        const Actor* pUser = Cast<Actor>(TESForm::GetById(it->second.UserFormId));
        // In the chest's cell, or outdoors in the same worldspace (a chest near a cell's edge).
        const TESObjectCELL* pUserCell = pUser ? pUser->GetParentCellEx() : nullptr;
        const TESObjectCELL* pChestCell = pChest ? pChest->GetParentCellEx() : nullptr;
        const bool cUserHere = pUserCell && pChestCell && (pUserCell == pChestCell || (pUserCell->worldspace && pUserCell->worldspace == pChestCell->worldspace));
        const float cDistance = cUserHere ? glm::distance(glm::vec3(pUser->position), glm::vec3(pChest->position)) : 0.f;
        if (aConnected && pChest && !ContainerLidRules::ShouldClose(cUserHere, cDistance, cNow - it->second.OpenedAt))
        {
            ++it;
            continue;
        }
        if (pChest && pChest->GetParentCellEx() && pChest->GetParentCellEx()->IsAttached())
            AnimateLid(pChest, false);
        spdlog::debug("ContainerLid: {:X} closed here; its user {:X} left it", it->first, it->second.UserFormId);
        it = s_openLids.erase(it);
    }
}
} // namespace

void ObjectService::OnActivate(const ActivateEvent& acEvent) noexcept
{
    // Looked up again: an item taken into an inventory is deleted, and its reference can be freed before this update.
    TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(acEvent.ObjectId));
    Actor* pActivator = Cast<Actor>(TESForm::GetById(acEvent.ActivatorId));
    if (!pObject || !pActivator)
        return;

    if (acEvent.ActivateFlag)
    {
        pObject->Activate(pActivator, acEvent.Unk1, acEvent.pObjectToGet, acEvent.Count, acEvent.DefaultProcessing);
    }

    // This game's actor used a trap's trigger, as a hinge door: the trap it fires is this game's to send.
    NoteTriggerTouched(pObject, pActivator);

    // Player furniture is shared once the engine has chosen its marker.
    if (pActivator == PlayerCharacter::Get() && pObject->baseForm && pObject->baseForm->formType == FormType::Furniture)
        return;

    if (!m_transport.IsConnected())
        return;

    // A temporary reference exists only in this game, and its id means nothing in the others.
    if (pObject->IsTemporary())
        return;

    // A locked object opens the lockpicking menu, not itself, so its activation is not sent.
    // Only the locked bit counts: a leveled lock keeps its flag once unlocked, and that door's openings were never sent.
    if (Lock* pLock = pObject->GetLock())
    {
        if (pLock->IsLocked())
            return;
    }

    ActivateRequest request;

    if (!m_world.GetModSystem().GetServerModId(pObject->formID, request.Id))
    {
        spdlog::error("Server form id not found for object form id {:X}", pObject->formID);
        return;
    }

    TESObjectCELL* pCell = pObject->GetParentCellEx();
    if (!pCell)
    {
        spdlog::error("Activated object has no parent cell: {:X}", pObject->formID);
        return;
    }

    if (!m_world.GetModSystem().GetServerModId(pCell->formID, request.CellId))
    {
        spdlog::error("Server cell id not found for cell form id {:X}", pCell->formID);
        return;
    }

    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    const auto pEntity = Utils::FindById<FormIdComponent>(view, pActivator->formID);

    if (!pEntity)
    {
        // spdlog::error("Activator entity not found for form id {:X}", pActivator->formID);
        return;
    }

    // Only this game's own actors: a remote copy's activation replays its owner's, and sending it back repeats it there.
    if (!m_world.all_of<LocalComponent>(*pEntity))
    {
        spdlog::info("Activation of {:X} by {:X}, another game's actor, not sent: its owner reports its own", pObject->formID, pActivator->formID);
        return;
    }

    // A replayed mechanism's script using the next one as this player: the other games did that themselves.
    if (pActivator == PlayerCharacter::Get() && ActivationCatchUpService::FollowsReplay(pObject))
    {
        spdlog::debug("ActivationCatchUp: {:X} used by this player's replay of another game's use; not sent", pObject->formID);
        return;
    }

    // A door whose activation a script blocks (BlockActivation) does not move here, so the other games must not open it either.
    // The script's own Activate with default processing only does move it, block or not, so that one is sent.
    const bool cDoor = pObject->baseForm && pObject->baseForm->formType == FormType::Door;
    if (cDoor && !acEvent.DefaultProcessing && acEvent.ActivationBlocked)
    {
        // A script can turn its door away again and again, so each door says so at most every 5 s.
        static Map<uint32_t, std::chrono::steady_clock::time_point> s_lastBlockedLog;
        const auto cNow = std::chrono::steady_clock::now();
        auto& last = s_lastBlockedLog[pObject->formID];
        if (cNow - last >= std::chrono::seconds(5))
        {
            last = cNow;
            spdlog::info("Door {:X} activated by {:X}, but a script blocks its activation; not sent", pObject->formID, pActivator->formID);
        }
        return;
    }

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(*pEntity);
    if (!serverIdRes.has_value())
        return;

    request.ActivatorId = serverIdRes.value();
    request.PreActivationOpenState = acEvent.PreActivationOpenState;

    m_transport.Send(request);

    // A lever, button or other activator this player used: what its script switches next goes to the party too.
    // Talking, taking an item or opening a chest is not one, so a member's own quests stay theirs.
    const auto cType = pObject->baseForm ? static_cast<uint8_t>(pObject->baseForm->formType) : 0;
    if (pActivator == PlayerCharacter::Get() && (cType == 24 || cType == 25))
        m_ownUseAt = std::chrono::steady_clock::now();

    if (pObject->baseForm && pObject->baseForm->formType == FormType::Door)
        spdlog::info(
            "Door {:X} ({:X}:{:X}) activated by {:X} from state {}; sent to the players in cell {:X}:{:X}", pObject->formID, request.Id.ModId, request.Id.BaseId,
            pActivator->formID, static_cast<int>(acEvent.PreActivationOpenState), request.CellId.ModId, request.CellId.BaseId);
}

void ObjectService::OnActivateNotify(const NotifyActivate& acMessage) noexcept
{
    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ActivatorId);

    // An activation by one of this game's actors is our own coming back from another game.
    // Replaying it would do it twice, putting the player back through a load door.
    if (const auto entity = Utils::FindEntityByServerId(acMessage.ActivatorId); pActor && (pActor == PlayerCharacter::Get() || (entity && m_world.all_of<LocalComponent>(*entity))))
    {
        spdlog::warn("Activation of {:X}:{:X} by {:X}, an actor of this game, came back from another game; not done again", acMessage.Id.ModId, acMessage.Id.BaseId, pActor->formID);
        return;
    }

    const uint32_t cObjectId = World::Get().GetModSystem().GetGameId(acMessage.Id);
    TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
    if (!pObject)
    {
        spdlog::error("Failed to retrieve object to activate: {:X}:{:X} (form {:X} here), activated by server id {:X}", acMessage.Id.ModId, acMessage.Id.BaseId, cObjectId,
                      acMessage.ActivatorId);
        return;
    }

    // Used in another game since this game's script moved it, so the kept state is the newer one again.
    m_doorsMovedHere.erase(pObject->formID);

    // Only an object in a cell this game has loaded is used here: a script's use of a far or indoor reference must not run its scripts here.
    // A door's state still comes when this game enters its cell, with the cell's objects.
    const TESObjectCELL* pCell = pObject->GetParentCellEx();
    if (!pCell || !pCell->IsAttached())
    {
        spdlog::info("Activation of {:X}, activated by server id {:X}: its cell is not loaded here; not done", pObject->formID, acMessage.ActivatorId);
        return;
    }

    // A door that does not lead elsewhere is set to the state it was left in over there, not used again here.
    // It needs no actor, so a door opened by an NPC this game has no copy of opens too.
    // Setting it is not an activation here, so nothing is sent back and two games cannot pass it to and fro.
    const auto cRemoteBefore = static_cast<TESObjectREFR::OpenState>(acMessage.PreActivationOpenState);
    const auto cRemoteAfter = static_cast<TESObjectREFR::OpenState>(DoorState::AfterActivation(cRemoteBefore));
    if (pObject->baseForm->formType == FormType::Door && !pObject->extraData.Contains(ExtraDataType::Teleport) && cRemoteAfter != TESObjectREFR::kNone)
    {
        const bool cOpen = cRemoteAfter == TESObjectREFR::kOpen;
        const TESObjectREFR::OpenState cLocal = pObject->GetOpenState();
        const Lock* pLock = pObject->GetLock();
        const bool cLockedHere = pLock && pLock->IsLocked();
        const std::string cBy = pActor ? fmt::format("{:X}", pActor->formID) : fmt::format("server id {:X}, not here", acMessage.ActivatorId);

        // Already there, or on its way.
        if (cLocal == cRemoteAfter || cLocal == (cOpen ? TESObjectREFR::kOpening : TESObjectREFR::kClosing))
        {
            spdlog::info("Door {:X}: {} in another game (by {}); already {} here", pObject->formID, cOpen ? "opened" : "closed", cBy, static_cast<int>(cLocal));
            return;
        }

        // Opening a door locked here unlocks it too (the game's SetOpen does): it was unlocked over there, since a locked door's use is not sent.
        pObject->SetOpen(cOpen);
        spdlog::info("Door {:X}: {} in another game (by {}, from state {}); set {} here from state {}{}", pObject->formID, cOpen ? "opened" : "closed", cBy,
                     static_cast<int>(cRemoteBefore), cOpen ? "open" : "closed", static_cast<int>(cLocal), cLockedHere && cOpen ? ", and unlocked: it was locked here" : "");
        return;
    }

    // Getting on a horse comes by the ride's own word, which puts the rider's copy on the horse here (MountService). Used here, the horse
    // would give the copy a mount package it never runs, and that half-started ride bars the horse to everybody else.
    if (const Actor* pMount = Cast<Actor>(pObject); pMount && pMount->IsMount())
    {
        spdlog::debug("Mount: use of horse {:X} by server id {:X} in another game not done here; its ride comes by its own word", pObject->formID, acMessage.ActivatorId);
        return;
    }

    if (!pActor)
    {
        spdlog::error("{}: could not find actor server id {:X} (object {:X})", __FUNCTION__, acMessage.ActivatorId, pObject->formID);
        return;
    }

    // Another game's actor through a load door: its copy here does not follow it, its owner's game reports where it went.
    // Activate calls the game's own activation, past the copy's refusal in HookActivate, and a copy standing at the door went straight through.
    // Taken out of this game's loaded cells, it was removed, and the server sent it again where it last stood: a player seen twice at a gate.
    const ActorExtension* pActorExtension = pActor->GetExtension();
    if (pActorExtension && pActorExtension->IsRemotePlayer() && pObject->baseForm && pObject->baseForm->formType == FormType::Furniture)
        return;
    // A use arms a sprung bear trap and springs an armed one, so the trap is set to the state its user's game left it in instead
    // (ReferenceStateService).
    if (pObject->baseForm && ReferenceStates::IsBearTrap(pObject->baseForm->formID))
        return;
    if (pObject->extraData.Contains(ExtraDataType::Teleport) && pActorExtension && pActorExtension->IsRemote())
    {
        spdlog::debug("DoorCopy: load door {:X} used by {:X} in its owner's game; not used here, its owner's game reports where it goes", pObject->formID, pActor->formID);
        return;
    }

    if (pObject->baseForm->formType == FormType::Door)
    {
        auto remotePreActivationState = static_cast<TESObjectREFR::OpenState>(acMessage.PreActivationOpenState);
        TESObjectREFR::OpenState localState = pObject->GetOpenState();
        const Lock* pLock = pObject->GetLock();
        const bool cLockedHere = pLock && pLock->IsLocked();

        if (remotePreActivationState != localState)
        {
            // The doors are unsynced at this point. If we'll Activate the one on our side
            // it'll just continue to be unsynced (open remotely, closed locally and vice versa)
            spdlog::info("Door {:X}: {:X} activated it in another game from state {}, here it is in state {}{}; left as it is", pObject->formID, pActor->formID,
                         static_cast<int>(remotePreActivationState), static_cast<int>(localState), cLockedHere ? " and locked" : "");
            return;
        }

        // Activated by the other player's copy, which has no key: a door locked here stays shut.
        spdlog::info("Door {:X}: {:X} activated it in another game from state {}; done here too{}", pObject->formID, pActor->formID,
                     static_cast<int>(remotePreActivationState), cLockedHere ? ", but it is locked here and will not open" : "");
    }

    // What a replayed lever's or button's script switches is the user's game's to send (SharedHomes::kReplayQuietMs).
    if (const auto cType = pObject->baseForm ? static_cast<uint8_t>(pObject->baseForm->formType) : 0; cType == 24 || cType == 25)
        m_replayAt = std::chrono::steady_clock::now();

    // Another player opened a chest in their game: its lid opens here too, and closes once they leave it (ContainerLidRules).
    if (pObject->baseForm && pObject->baseForm->formType == FormType::Container && pActorExtension && pActorExtension->IsRemotePlayer())
        OpenLidFor(pObject, pActor);

    // unsure if these flags are the best, but these are passed with the papyrus Activate fn
    // might be an idea to have the client send the flags through NotifyActivate
    pObject->Activate(pActor, 0, nullptr, 1, 0);
}

void ObjectService::OnScriptDoorEvent(const ScriptDoorEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    // The leader's scripts' doors are shared, and a member's just after its player used a lever or button (SharedHomes).
    // A member's own other scripts stay its own, and a state the server kept from an older use must not undo them.
    if (!SharesSwitch())
    {
        m_doorsMovedHere.insert(acEvent.FormId);
        return;
    }
    m_doorsMovedHere.erase(acEvent.FormId);

    // A load door is used in the other games, not set, and using it would take the leader's copy through.
    auto* pDoor = Cast<TESObjectREFR>(TESForm::GetById(acEvent.FormId));
    if (!pDoor || pDoor->extraData.Contains(ExtraDataType::Teleport))
        return;

    const TESObjectCELL* pCell = pDoor->GetParentCellEx();
    auto& modSystem = m_world.GetModSystem();
    ActivateRequest request;
    if (!pCell || !modSystem.GetServerModId(pDoor->formID, request.Id) || !modSystem.GetServerModId(pCell->formID, request.CellId))
        return;

    // Sent as the player's use of the door: the other games set the door from that, and the server keeps its state.
    const auto view = m_world.view<FormIdComponent, LocalComponent>();
    const auto cPlayer = Utils::FindById<FormIdComponent>(view, 0x14);
    if (!cPlayer)
        return;

    request.ActivatorId = view.get<LocalComponent>(*cPlayer).Id;
    request.PreActivationOpenState = acEvent.Open ? DoorState::kClosed : DoorState::kOpen;
    m_transport.Send(request);

    spdlog::info("Door {:X} ({:X}:{:X}) {} by a script here; sent to the players in cell {:X}:{:X}", pDoor->formID, request.Id.ModId, request.Id.BaseId,
                 acEvent.Open ? "opened" : "closed", request.CellId.ModId, request.CellId.BaseId);
}

void ObjectService::OnLockChange(const LockChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    // A lock set from another game comes back here through the game's own lock change; sent back, two games would pass it to and fro.
    if (const auto it = m_lockFromOtherGame.find(acEvent.FormId); it != m_lockFromOtherGame.end())
    {
        const bool cEcho = it->second.first == acEvent.IsLocked && std::chrono::steady_clock::now() - it->second.second < std::chrono::seconds(2);
        m_lockFromOtherGame.erase(it);
        if (cEcho)
            return;
    }

    LockChangeRequest request;

    if (!m_world.GetModSystem().GetServerModId(acEvent.FormId, request.Id))
    {
        spdlog::error("Server form id for lock object not found, form id: {:X}", acEvent.FormId);
        return;
    }

    const auto* const pObject = Cast<TESObjectREFR>(TESForm::GetById(acEvent.FormId));
    if (!pObject)
        return;

    TESObjectCELL* pCell = pObject->GetParentCellEx();
    if (!pCell)
    {
        spdlog::error("Activated object has no parent cell: {:X}", pObject->formID);
        return;
    }

    // A lock that changes in a cell this game has not loaded is the game's own doing (a shop locked for the night), which every game does for itself.
    if (!pCell->IsAttached())
    {
        spdlog::debug("Lock: {:X} {} here in cell {:X}, which is not loaded; not sent", pObject->formID, acEvent.IsLocked ? "locked" : "unlocked", pCell->formID);
        return;
    }

    if (!m_world.GetModSystem().GetServerModId(pCell->formID, request.CellId))
    {
        spdlog::error("Server cell id for cell not found, cell form id: {:X}", pCell->formID);
        return;
    }

    request.IsLocked = acEvent.IsLocked;
    request.LockLevel = acEvent.LockLevel;

    m_transport.Send(request);
    spdlog::info("Lock: {:X} {} here (level {}); sent to the players in cell {:X}:{:X}", pObject->formID, acEvent.IsLocked ? "locked" : "unlocked", acEvent.LockLevel,
                 request.CellId.ModId, request.CellId.BaseId);
}

void ObjectService::OnLockChangeNotify(const NotifyLockChange& acMessage) noexcept
{
    const auto cObjectId = World::Get().GetModSystem().GetGameId(acMessage.Id);
    if (cObjectId == 0)
    {
        spdlog::error("Failed to retrieve object id to (un)lock.");
        return;
    }

    auto* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
    if (!pObject)
    {
        spdlog::error("Failed to retrieve object to (un)lock.");
        return;
    }

    auto* pLock = pObject->GetLock();

    // Already as the other game has it.
    const bool cLockedHere = pLock && pLock->IsLocked();
    if (cLockedHere == acMessage.IsLocked && (!cLockedHere || pLock->lockLevel == acMessage.LockLevel))
    {
        spdlog::info("Lock: {:X} {} in another game; already so here", pObject->formID, acMessage.IsLocked ? "locked" : "unlocked");
        return;
    }
    spdlog::info("Lock: {:X} {} in another game; done here", pObject->formID, acMessage.IsLocked ? "locked" : "unlocked");

    if (!pLock && acMessage.IsLocked)
    {
        pLock = pObject->CreateLock();
        if (!pLock)
        {
            spdlog::error("Failed to create lock for object form id {:X}", pObject->formID);
            return;
        }
    }

    pLock->lockLevel = acMessage.LockLevel;
    pLock->SetLock(acMessage.IsLocked);
    m_lockFromOtherGame[pObject->formID] = {acMessage.IsLocked, std::chrono::steady_clock::now()};
    pObject->LockChange();
}

void ObjectService::OnScriptAnimationEvent(const ScriptAnimationEvent& acEvent) noexcept
{
    ScriptAnimationRequest request{};
    request.FormID = acEvent.FormID;
    request.Animation = acEvent.Animation;
    request.EventName = acEvent.EventName;

    m_transport.Send(request);
}

void ObjectService::OnNotifyScriptAnimation(const NotifyScriptAnimation& acMessage) noexcept
{
    if (acMessage.FormID == 0)
        return;

    auto* pForm = TESForm::GetById(acMessage.FormID);
    auto* pObject = Cast<TESObjectREFR>(pForm);

    if (!pObject)
    {
        spdlog::error("Failed to fetch notify script animation object, form id: {:X}", acMessage.FormID);
        return;
    }

    BSFixedString eventName(acMessage.EventName.c_str());
    if (acMessage.Animation == String{})
    {
        pObject->PlayAnimation(&eventName);
    }
    else
    {
        BSFixedString animation(acMessage.Animation.c_str());
        pObject->PlayAnimationAndWait(&animation, &eventName);
    }
}

void ObjectService::NoteTriggerTouched(const TESObjectREFR* apTrigger, const TESObjectREFR* apActionRef) noexcept
{
    if (!apTrigger || !apTrigger->baseForm || !TrapSync::IsTriggerBase(apTrigger->baseForm->formID))
        return;

    Actor* pActor = Cast<Actor>(apActionRef);
    const ActorExtension* pExtension = pActor ? pActor->GetExtension() : nullptr;
    if (pExtension && pExtension->IsLocal())
        m_traps.Touched(apTrigger->formID, std::chrono::steady_clock::now());
}

void ObjectService::NoteTouchInLedger(const uint32_t aTriggerId, const TESObjectREFR* apActionRef) noexcept
{
    // Read on the game's thread: only whether the actor is this game's or a copy, which its extension says.
    Actor* pActor = Cast<Actor>(apActionRef);
    const ActorExtension* pExtension = pActor ? pActor->GetExtension() : nullptr;
    if (!pExtension || (!pExtension->IsLocal() && !pExtension->IsRemote()))
        return;

    TrapSync::SharedLedger().Touched(aTriggerId, pExtension->IsRemote(), std::chrono::steady_clock::now());
}

void ObjectService::OnTrapFire(const TrapFireEvent& acEvent) noexcept
{
    const auto cNow = std::chrono::steady_clock::now();
    m_traps.Forget(cNow);

    // Fired here, for this game's actor or another's: another game's word for it soon after is the same firing.
    const bool cTouchedHere = m_traps.IsTouchedHere(acEvent.TriggerId, cNow);
    m_traps.Fired(acEvent.TrapId, cNow);
    if (!cTouchedHere || !m_transport.IsConnected())
    {
        spdlog::debug(
            "Trap: {:X} fired here by trigger {:X}; not sent{}", acEvent.TrapId, acEvent.TriggerId, cTouchedHere ? ", not connected" : ": no actor of this game set it off");
        return;
    }

    // A temporary reference exists only in this game.
    TESObjectREFR* pTrap = Cast<TESObjectREFR>(TESForm::GetById(acEvent.TrapId));
    TESObjectREFR* pTrigger = Cast<TESObjectREFR>(TESForm::GetById(acEvent.TriggerId));
    if (!pTrap || !pTrigger || pTrap->IsTemporary() || pTrigger->IsTemporary())
        return;

    TESObjectCELL* pCell = pTrap->GetParentCellEx();
    auto& modSystem = m_world.GetModSystem();
    RequestTrapFire request;
    if (!pCell || !modSystem.GetServerModId(pTrap->formID, request.Id) || !modSystem.GetServerModId(pTrigger->formID, request.TriggerId) ||
        !modSystem.GetServerModId(pCell->formID, request.CellId))
    {
        spdlog::debug("Trap: {:X} fired by trigger {:X}, but they have no server ids; not sent", pTrap->formID, pTrigger->formID);
        return;
    }

    if (const TESWorldSpace* pWorldSpace = pTrap->GetWorldSpace(); pWorldSpace && modSystem.GetServerModId(pWorldSpace->formID, request.WorldSpaceId))
        request.CenterCoords = GridCellCoords::CalculateGridCellCoords(pTrap->position.x, pTrap->position.y);

    m_transport.Send(request);
    spdlog::debug("Trap: {:X} fired by trigger {:X}, which this game's actor set off; sent to the players near it", pTrap->formID, pTrigger->formID);
}

void ObjectService::OnTrapFireNotify(const NotifyTrapFire& acMessage) noexcept
{
    auto& modSystem = m_world.GetModSystem();
    TESObjectREFR* pTrap = Cast<TESObjectREFR>(TESForm::GetById(modSystem.GetGameId(acMessage.Id)));
    TESObjectREFR* pTrigger = Cast<TESObjectREFR>(TESForm::GetById(modSystem.GetGameId(acMessage.TriggerId)));
    if (!pTrap || !pTrigger)
    {
        spdlog::debug(
            "Trap: {:X}:{:X} fired in another game, by trigger {:X}:{:X}; not found here", acMessage.Id.ModId, acMessage.Id.BaseId, acMessage.TriggerId.ModId,
            acMessage.TriggerId.BaseId);
        return;
    }

    // Only a trap in a cell this game has loaded fires here, as with activations.
    const TESObjectCELL* pCell = pTrap->GetParentCellEx();
    if (!pCell || !pCell->IsAttached())
    {
        spdlog::debug("Trap: {:X} fired in another game; its cell is not loaded here", pTrap->formID);
        return;
    }

    // The other player's copy set it off here first, however long ago within the pairing window: this word is that step's.
    const auto cNow = std::chrono::steady_clock::now();
    if (TrapSync::SharedLedger().TakeCopyFire(pTrap->formID, cNow))
    {
        spdlog::debug("Trap: {:X} fired in another game; the stepper's copy fired it here already, so not again", pTrap->formID);
        return;
    }
    // Fired here lately for another reason.
    if (m_traps.IsFiredRecently(pTrap->formID, cNow))
    {
        spdlog::debug("Trap: {:X} fired in another game; it fired here within 2 s, so not again", pTrap->formID);
        return;
    }
    if (pTrap != pTrigger && m_traps.IsReplayed(pTrigger->formID, cNow))
    {
        spdlog::debug("Trap: {:X} fired in another game; its trigger {:X} fired it here already", pTrap->formID, pTrigger->formID);
        return;
    }

    // Its own script fires it, as its trigger would here: each game's hits land on that game's own actors only.
    m_traps.Fired(pTrap->formID, cNow);
    if (pTrap == pTrigger)
        m_traps.Replayed(pTrigger->formID, cNow);
    spdlog::debug("Trap: {:X} fired in another game by trigger {:X}; fired here too", pTrap->formID, pTrigger->formID);
    // The copy's own firing for this step, when it steps on the trigger here later, is dropped in the Activate hook.
    TrapSync::SharedLedger().WordFired(pTrap->formID, cNow);
    TrapSync::ScopedFiringForWord firingForWord;
    pTrap->Activate(pTrigger, 0, nullptr, 1, false);
}

BSTEventResult ObjectService::OnEvent(const TESTriggerEnterEvent* acpEvent, const EventDispatcher<TESTriggerEnterEvent>*)
{
    // Fired on the game's own threads: noted with the world's update, where the traps are handled.
    // The ledger is told at once, as the trigger's script may fire its trap before that update.
    if (acpEvent && acpEvent->pTrigger && acpEvent->pTrigger->baseForm && TrapSync::IsTriggerBase(acpEvent->pTrigger->baseForm->formID) && acpEvent->pActionRef)
        NoteTouchInLedger(acpEvent->pTrigger->formID, acpEvent->pActionRef);
    if (acpEvent && acpEvent->pTrigger && acpEvent->pTrigger->baseForm && TrapSync::IsTriggerBase(acpEvent->pTrigger->baseForm->formID) && acpEvent->pActionRef)
        m_world.GetRunner().Queue([this, cTriggerId = acpEvent->pTrigger->formID, cActorId = acpEvent->pActionRef->formID]()
                                  { NoteTriggerTouched(Cast<TESObjectREFR>(TESForm::GetById(cTriggerId)), Cast<TESObjectREFR>(TESForm::GetById(cActorId))); });

    return BSTEventResult::kOk;
}

// A plate that fires when its weight is taken off fires as the actor leaves it.
BSTEventResult ObjectService::OnEvent(const TESTriggerLeaveEvent* acpEvent, const EventDispatcher<TESTriggerLeaveEvent>*)
{
    if (acpEvent && acpEvent->pTrigger && acpEvent->pTrigger->baseForm && TrapSync::IsTriggerBase(acpEvent->pTrigger->baseForm->formID) && acpEvent->pActionRef)
        NoteTouchInLedger(acpEvent->pTrigger->formID, acpEvent->pActionRef);
    if (acpEvent && acpEvent->pTrigger && acpEvent->pTrigger->baseForm && TrapSync::IsTriggerBase(acpEvent->pTrigger->baseForm->formID) && acpEvent->pActionRef)
        m_world.GetRunner().Queue([this, cTriggerId = acpEvent->pTrigger->formID, cActorId = acpEvent->pActionRef->formID]()
                                  { NoteTriggerTouched(Cast<TESObjectREFR>(TESForm::GetById(cTriggerId)), Cast<TESObjectREFR>(TESForm::GetById(cActorId))); });

    return BSTEventResult::kOk;
}

BSTEventResult ObjectService::OnEvent(const TESActivateEvent* acEvent, const EventDispatcher<TESActivateEvent>* aDispatcher)
{
#if ENVIRONMENT_DEBUG
    auto view = m_world.view<ObjectComponent>();

    const auto itor = Utils::FindById<ObjectComponent>(view, acEvent->object->formID);

    if (!itor)
    {
        AddObjectComponent(acEvent->object);
    }
#endif

    return BSTEventResult::kOk;
}

namespace
{
// The kinds of reference a quest's scripts switch for the whole party: markers and other statics, activators (a siege's
// catapults), containers, doors, trees, plants and furniture.
// Actors, items, lights and temporary references are synced their own way, or not at all.
// SkyrimTogether.esp's references, the players' map markers, are each game's own.
// The same form in another game marks someone else, and moving one switches it off and on.
// DynDOLOD's are too: its scripts switch them by this player's distance, and each player generates a different file.
bool IsReferenceOf(const Mod* apPlugin, const uint32_t aFormId) noexcept
{
    if (!apPlugin || !apPlugin->IsLoaded())
        return false;
    if (apPlugin->IsLite())
        return (aFormId >> 24) == 0xFE && ((aFormId >> 12) & 0xFFF) == apPlugin->liteId;
    return (aFormId >> 24) == apPlugin->standardId;
}

bool IsOwnPluginReference(uint32_t aFormId) noexcept
{
    static const Mod* s_pMarkers = ModManager::Get()->GetByName("SkyrimTogether.esp");
    static const Mod* s_pLodPlugin = ModManager::Get()->GetByName("DynDOLOD.esp");
    static const Mod* s_pLodMaster = ModManager::Get()->GetByName("DynDOLOD.esm");
    return IsReferenceOf(s_pMarkers, aFormId) || IsReferenceOf(s_pLodPlugin, aFormId) || IsReferenceOf(s_pLodMaster, aFormId);
}

// A trigger box: an activator with a primitive shape, which the game fires when something walks into it.
// Its script plays its effect in that game only (a hall's collapse, a scene's start), sets its stage, and often switches the box off.
// Each game keeps its own, so every player still fires it on arrival; the stage it sets goes once per party.
bool IsTriggerVolume(const TESObjectREFR* apRef) noexcept
{
    return apRef && apRef->baseForm && static_cast<uint8_t>(apRef->baseForm->formType) == 24 && apRef->extraData.Contains(ExtraDataType::Primitive);
}

bool IsSharedSwitchKind(const TESObjectREFR* apRef) noexcept
{
    if (!apRef || !apRef->baseForm || apRef->IsTemporary() || IsOwnPluginReference(apRef->formID) || IsTriggerVolume(apRef))
        return false;
    switch (static_cast<uint8_t>(apRef->baseForm->formType))
    {
    case 24: // activator
    case 25: // talking activator
    case 28: // container
    case 29: // door
    case 31: // light
    case 34: // static, which the markers a quest's references hang from are
    case 36: // movable static
    case 38: // tree
    case 39: // flora
    case 40: // furniture
        return true;
    default: return GameExtension::SharedSwitch(apRef->formID, apRef->baseForm->formID, static_cast<uint32_t>(apRef->baseForm->formType));
    }
}

// An actor reaches the leader's switches only through ObjectService::SwitchActor.
// So one arriving here was handed over by a plugin, and the game's own scripts never send one.
bool IsSwitchableActor(TESObjectREFR* apRef) noexcept
{
    return apRef && !apRef->IsTemporary() && apRef != PlayerCharacter::Get() && Cast<Actor>(apRef);
}

// A dead actor whose reference a master file holds (the game's own people, the DLCs', the patches') stays on.
// The game's body clean-up empties such a body later, and emptying a switched-off body crashes the game.
// Plugins keep the actors they place in plugins of their own, or spawn them, so their dead still go.
bool IsDeadMasterFileActor(TESObjectREFR* apRef) noexcept
{
    const Actor* pActor = Cast<Actor>(apRef);
    if (!pActor || !pActor->IsDead())
        return false;
    for (const Mod* pMod : ModManager::Get()->mods)
    {
        // Bit 0 of a file's header flags marks a master file.
        if (IsReferenceOf(pMod, apRef->formID))
            return (pMod->flags & 1) != 0;
    }
    return false;
}

// The game owns an actor while it loads or disposes of one, and disabling it then crashes.
// A rebuild of the actor would also undo the switch, so the switch waits for all of these.
bool IsSettledActor(TESObjectREFR* apRef) noexcept
{
    UI* pUI = UI::Get();
    if ((pUI && pUI->GetMenuOpen(BSFixedString("Loading Menu"))) || apRef->IsStillLoading() || (!apRef->GetNiNode() && !apRef->IsDisabled()))
        return false;
    const ActorExtension* pExtension = Cast<Actor>(apRef)->GetExtension();
    return pExtension && pExtension->Reconciliation == ActorExtension::ReconciliationStage::None;
}

// How the leader's switches went here since the last line about them.
size_t s_enableSwitched = 0;
size_t s_enableAlreadySo = 0;
size_t s_enableLeftAlone = 0;

// At most one line per kind every 10 s, with how many switches went by since the last one.
bool LogDue(std::chrono::steady_clock::time_point& aNext) noexcept
{
    const auto cNow = std::chrono::steady_clock::now();
    if (cNow < aNext)
        return false;
    aNext = cNow + std::chrono::seconds(10);
    return true;
}
} // namespace

bool ObjectService::SharesSwitch() const noexcept
{
    // The quest catch-up replays the party's past: what its stages switch, the other games already have (the leader's too).
    if (m_world.ctx().at<QuestService>().CatchUpHoldsSwitches())
        return false;
    const auto cNow = std::chrono::steady_clock::now();
    const auto cSince = [cNow](const std::chrono::steady_clock::time_point acAt) noexcept
    {
        if (acAt == std::chrono::steady_clock::time_point{})
            return SharedHomes::kNever;
        const auto cMs = std::chrono::duration_cast<std::chrono::milliseconds>(cNow - acAt).count();
        return cMs < 0 ? uint64_t{0} : static_cast<uint64_t>(cMs);
    };
    const auto cSinceTicks = [&cSince](const std::atomic<int64_t>& acTicks) noexcept
    {
        const int64_t cTicks = acTicks.load(std::memory_order_relaxed);
        return cSince(cTicks == 0 ? std::chrono::steady_clock::time_point{} : std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(cTicks)));
    };
    return SharedHomes::SharesSwitch(m_world.GetPartyService().IsLeader(), cSince(m_ownUseAt), cSince(m_replayAt), cSinceTicks(m_ownStepAt), cSinceTicks(m_questReplayAt));
}

void ObjectService::OnReferenceEnableEvent(const ReferenceEnableEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    // A switch applied here from another game is not sent back: two games would pass it to and fro.
    if (const auto it = m_appliedSwitches.find(acEvent.FormId); it != m_appliedSwitches.end())
    {
        const bool cApplied = it->second == acEvent.Enabled;
        m_appliedSwitches.erase(it);
        if (cApplied)
            return;
    }

    // The leader's game runs the party's quests, so its switches are shared; a member's just after its player used a lever or button,
    // or just after its own quest reached a step (SharedHomes::SharesSwitch). The server takes a member's only for a step new to the party.
    if (!SharesSwitch())
        return;
    if (IsSharedSwitchKind(Cast<TESObjectREFR>(TESForm::GetById(acEvent.FormId))))
        m_enableChanges.emplace_back(acEvent.FormId, acEvent.Enabled);
}

bool ObjectService::ApplyReferenceEnable(uint32_t aFormId, bool aEnabled) noexcept
{
    auto* pRef = Cast<TESObjectREFR>(TESForm::GetById(aFormId));
    if (!pRef)
        return false;
    // A switch an older leader sent, or one kept on the server from before, leaves a trigger box as it is here.
    if (IsTriggerVolume(pRef))
    {
        // Logged only when the leader's switch differs here: a join replay can hold hundreds of them.
        if (aEnabled == pRef->IsDisabled())
            spdlog::info("ReferenceEnable: trigger {:X} (base {:X}) left {} here; each game's trigger fires for its own player", pRef->formID, pRef->baseForm->formID,
                         pRef->IsDisabled() ? "off" : "on");
        ++s_enableLeftAlone;
        return true;
    }
    // The leader sends only these kinds; another plugin at the same id here is left alone.
    if ((!IsSharedSwitchKind(pRef) && !IsSwitchableActor(pRef)) || pRef->IsDeleted())
    {
        ++s_enableLeftAlone;
        return true;
    }
    // An actor the leader's game switched on follows its owner's life here, whether this game switches it now, later, or has it on already.
    if (aEnabled && IsSwitchableActor(pRef))
        m_world.GetCharacterService().FollowOwnersLife(aFormId);
    if (aEnabled == !pRef->IsDisabled())
    {
        ++s_enableAlreadySo;
        return true;
    }
    const bool cActor = IsSwitchableActor(pRef);
    if (cActor && !aEnabled && IsDeadMasterFileActor(pRef))
    {
        spdlog::info("ReferenceEnable: dead {:X} (base {:X}) left on here, as the game's body clean-up may still empty it", pRef->formID, pRef->baseForm->formID);
        ++s_enableLeftAlone;
        return true;
    }
    if (cActor && !IsSettledActor(pRef))
        return false;

    // A door, container, activator, furniture or actor that comes or goes is one a player can see missing.
    // Markers and statics switch by the hundred and are left out.
    const auto cType = static_cast<uint8_t>(pRef->baseForm->formType);
    if (cType == 24 || cType == 25 || cType == 28 || cType == 29 || cType == 40 || cActor)
        spdlog::info(
            "ReferenceEnable: {:X} (base {:X}, form type {}) switched {} here, as another game of the party has it", pRef->formID, pRef->baseForm->formID, cType,
            aEnabled ? "on" : "off");

    m_appliedSwitches[aFormId] = aEnabled;
    if (aEnabled)
        pRef->EnableImpl();
    else
        pRef->Disable();
    if (cActor && aEnabled)
        m_world.GetCharacterService().LeaderSwitchedOn(aFormId);

    // An actor's disable is only queued, so it has not landed yet.
    if (aEnabled == !pRef->IsDisabled() || cActor)
        ++s_enableSwitched;
    else
        spdlog::warn("ReferenceEnable: {:X} (base {:X}) did not take the leader's switch {}", pRef->formID, pRef->baseForm->formID, aEnabled ? "on" : "off");
    return true;
}

void ObjectService::SwitchActor(uint32_t aFormId, bool aEnabled) noexcept
{
    // Actors stay the leader's to switch: the leader's game announces an actor it switched on, and a member's would make a second one.
    if (!m_transport.IsConnected() || !m_world.GetPartyService().IsLeader())
        return;

    // Every game leaves such a body on, so the server is not told either: it would stop syncing the body.
    if (!aEnabled && IsDeadMasterFileActor(Cast<TESObjectREFR>(TESForm::GetById(aFormId))))
    {
        spdlog::info("ReferenceEnable: dead {:X} not switched off for the party, as the game's body clean-up may still empty it", aFormId);
        return;
    }

    ReferenceEnableState state{};
    state.Enabled = aEnabled;
    if (!m_world.GetModSystem().GetServerModId(aFormId, state.Id.ModId, state.Id.BaseId))
    {
        spdlog::warn("ReferenceEnable: no server id for actor {:X}, so it was not switched", aFormId);
        return;
    }

    // Sent before the switch here: an actor switched on is announced by this game, and the server must know by then.
    RequestReferenceEnable request{};
    request.Changes.push_back(state);
    m_transport.Send(request);

    if (!ApplyReferenceEnable(aFormId, aEnabled))
        m_enableWaiting[aFormId] = aEnabled;
}

void ObjectService::OnNotifyReferenceEnable(const NotifyReferenceEnable& acMessage) noexcept
{
    size_t applied = 0;
    for (const auto& change : acMessage.Changes)
    {
        const uint32_t cFormId = m_world.GetModSystem().GetGameId(change.Id);
        if (!cFormId)
            continue;
        if (ApplyReferenceEnable(cFormId, change.Enabled))
        {
            ++applied;
            m_enableWaiting.erase(cFormId);
        }
        else
            m_enableWaiting[cFormId] = change.Enabled;
    }

    static std::chrono::steady_clock::time_point s_nextLog{};
    static size_t s_since = 0;
    s_since += acMessage.Changes.size();
    if (LogDue(s_nextLog))
    {
        spdlog::info(
            "ReferenceEnable: {} reference(s) the party's scripts switched (first {:X}:{:X} {}), {} applied here now, {} waiting for their cell; "
            "since the last such line {} switched, {} already so, {} left alone",
            s_since, acMessage.Changes.empty() ? 0 : acMessage.Changes[0].Id.ModId, acMessage.Changes.empty() ? 0 : acMessage.Changes[0].Id.BaseId,
            !acMessage.Changes.empty() && acMessage.Changes[0].Enabled ? "on" : "off", applied, m_enableWaiting.size(), s_enableSwitched, s_enableAlreadySo, s_enableLeftAlone);
        s_since = 0;
        s_enableSwitched = 0;
        s_enableAlreadySo = 0;
        s_enableLeftAlone = 0;
    }
}

void ObjectService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    PerfScope perfScope("ObjectService::OnUpdate");

    RunVendorChest();
    RunContainerLids(m_transport.IsConnected());

    if (!m_enableChanges.empty())
    {
        // A script can switch a reference off and on again within a frame, over and over.
        // Only where it ended goes out, or every other game would redo each switch.
        RequestReferenceEnable request{};
        std::unordered_set<uint32_t> done;
        for (auto it = m_enableChanges.rbegin(); it != m_enableChanges.rend(); ++it)
        {
            ReferenceEnableState state{};
            if (done.insert(it->first).second && m_world.GetModSystem().GetServerModId(it->first, state.Id.ModId, state.Id.BaseId))
            {
                state.Enabled = it->second;
                request.Changes.push_back(state);
            }
        }
        m_enableChanges.clear();

        if (!request.Changes.empty())
        {
            static std::chrono::steady_clock::time_point s_nextLog{};
            static size_t s_since = 0;
            s_since += request.Changes.size();
            if (LogDue(s_nextLog))
            {
                spdlog::info("ReferenceEnable: {} reference(s) switched by this game's scripts sent to the party (first {:X}:{:X} {})", s_since, request.Changes[0].Id.ModId,
                             request.Changes[0].Id.BaseId, request.Changes[0].Enabled ? "on" : "off");
                s_since = 0;
            }
            m_transport.Send(request);
        }
    }

    // References whose cell was not loaded when the leader's switch came are tried again every 2 s.
    if (m_enableWaiting.empty())
        return;
    m_enableRetry -= acEvent.Delta;
    if (m_enableRetry > 0.0)
        return;
    m_enableRetry = 2.0;
    for (auto it = m_enableWaiting.begin(); it != m_enableWaiting.end();)
        it = ApplyReferenceEnable(it->first, it->second) ? m_enableWaiting.erase(it) : std::next(it);
}
