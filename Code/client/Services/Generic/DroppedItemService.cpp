#include <Services/DroppedItemService.h>
#include <Helpers/PerfScope.h>

#include <World.h>
#include <Components.h>
#include <Actor.h>
#include <Services/TransportService.h>
#include <Services/ReferenceAngles.h>
#include <Services/PosedHands.h>
#include <Services/HeldMover.h>
#include <Services/SendGrid.h>

#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/CellChangeEvent.h>
#include <Events/DroppedItemEvents.h>
#include <Events/ItemTakeEvent.h>

#include <Messages/RequestDroppedItemAdd.h>
#include <Messages/RequestDroppedItemRemove.h>
#include <Messages/NotifyDroppedItem.h>
#include <Messages/NotifyDroppedItemRemoved.h>
#include <Messages/RequestDroppedItemMove.h>
#include <Messages/NotifyDroppedItemMove.h>
#include <Messages/RequestWorldObjectMove.h>
#include <Messages/NotifyWorldObjectMove.h>
#include <Messages/RequestPropBodies.h>
#include <Messages/NotifyPropBodies.h>
#include <Events/UpdateEvent.h>
#include <NetImmerse/NiTransform.h>
#include <NetImmerse/NiGlm.h>
#include <Helpers/ReadableMemory.h>

#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Games/TES.h>
#include <Services/ChestSelection.h>
#include <PlayerCharacter.h>
#include <ExtraData/ExtraCount.h>

#include <Games/Overrides.h>
#include <Misc/BSFixedString.h>

#include <cstdlib>
#include <limits>

#include <Games/Skyrim/PropHavok.h>
#include <Services/CopyWorkGate.h>
#include <Services/GameExtension.h>
#include <Interface/UI.h>

#include <ItemExtraData.h>
#include <ClientTrust.h>
#include <DroppedItemIdentity.h>
#include <Services/DroppedItemReplies.h>
#include <Games/Skyrim/HavokLayout.h>
#include <SharedHomes.h>

#ifdef SKYRIMVR
#include <Games/Skyrim/VR/VRBodySync.h>
#endif

namespace
{
//! How close a floor item must be to the server's position to count as the same item.
//! The item may have rolled or been reloaded since the drop.
constexpr float kSameItemRadius = 200.f;

//! Outdoors, how far from the player an item can be and still be in a loaded cell.
//! The loaded grid is 5 by 5 cells of 4096 units, two cells either side of the player's.
constexpr float kLoadedRadius = 8192.f;

const Vector<FormType> kItemTypes{FormType::Weapon, FormType::Armor, FormType::Ammo, FormType::Book, FormType::Ingredient, FormType::Alchemy,
                                  FormType::Misc, FormType::Key, FormType::SoulGem, FormType::Scroll, FormType::Light};

bool IsItem(const TESObjectREFR* apRef) noexcept
{
    if (!apRef || !apRef->baseForm)
        return false;
    for (const FormType type : kItemTypes)
        if (apRef->baseForm->formType == type)
            return true;
    return false;
}

bool ReadFloorItem(TESObjectREFR* apRef, Inventory::Entry& aItem) noexcept
{
    if (!IsItem(apRef) || apRef->IsDisabled() || apRef->IsDeleted() || !ItemExtraData::SetBase(aItem, apRef->baseForm))
        return false;
    aItem.Count = 1;
    if (auto* pCount = static_cast<ExtraCount*>(apRef->extraData.GetByType(ExtraDataType::Count)))
        aItem.Count = std::max<int32_t>(1, pCount->count);
    TESObjectREFR::GetItemFromExtraData(aItem, &apRef->extraData);
    return true;
}

float DistanceTo(const TESObjectREFR* apRef, const glm::vec3& acPosition) noexcept
{
    return glm::distance(glm::vec3(apRef->position.x, apRef->position.y, apRef->position.z), acPosition);
}

//! Whether the reference behind an id is still the item that was mapped.
//! A mapping made before the base was known (0) is trusted; anything else must match.
template <class T> bool IsStillOurs(const TESObjectREFR* apRef, const T& acMotion) noexcept
{
    return acMotion.BaseFormId == 0 || (apRef->baseForm && apRef->baseForm->formID == acMotion.BaseFormId);
}

//! Movement worth sending: a couple of units or a few degrees of turn; less is physics jitter.
constexpr float kMovedDistance = 2.f;
constexpr float kMovedAngle = 0.05f;

//! A resting world object starts moving again only on a clear move.
//! Physics settling and a still hand stay under this, and each restart holds the object in the other games again.
constexpr float kRestartDistance = 8.f;
constexpr float kRestartAngle = 0.3f;
// A move from the other side further than this is a jump (a load, a teleport), made with MoveTo; nearer ones move the 3D in place.
constexpr float kMoveInPlaceDistance = 1000.f;

// Whether a script holds the item still where it lies (a weapon rack's weapon), read from its first Havok body.
bool IsHeldStill(TESObjectREFR* apRef) noexcept
{
    bool heldStill = false;
    NiTree::VisitTree(
        apRef->GetNiNode(), 64,
        [&](void* apObject)
        {
            const uint8_t* pBody = HavokLayout::HavokBodyOf(apObject);
            if (!pBody)
                return true;
            heldStill = SharedHomes::IsHeldStillMotion(pBody[HavokLayout::kMotionTypeOffset]);
            return false;
        });
    return heldStill;
}

// An item the server says a script holds still where it lies: put there and held still once its 3D is there (HoldFixed).
template <class TMotion> void HoldStillWhenFixed(TMotion& aMotion, const NotifyDroppedItem& acMessage) noexcept
{
    if (!acMessage.Fixed || aMotion.OwnDrop)
        return;
    aMotion.RemoteFixed = true;
    aMotion.WasRemoteFixed = true;
    aMotion.FixPending = true;
    aMotion.LastPosition = glm::vec3(acMessage.Position);
    aMotion.LastRotation = acMessage.Rotation;
}

// Puts an object the other side moves where it says, without building its 3D again when it only moved a little.
void PlaceMovedObject(TESObjectREFR* apRef, const glm::vec3& acPosition, const glm::vec3& acRotation) noexcept
{
    apRef->SetRotation(acRotation.x, acRotation.y, acRotation.z);
    const NiPoint3 cTarget(acPosition);
    const glm::vec3 cNow(apRef->position.x, apRef->position.y, apRef->position.z);
    if (apRef->GetNiNode() && glm::distance(cNow, acPosition) < kMoveInPlaceDistance)
    {
        // That moves only the reference: its 3D and collision are moved to it too, warped there.
        apRef->SetPositionInPlace(cTarget);
        apRef->Update3DPosition(true);
    }
    else if (TESObjectCELL* pCell = apRef->GetParentCellEx())
        apRef->MoveTo(pCell, cTarget);
}

//! Where an item really is: its node's world transform follows physics, while the reference's position may lag.
//! Read from NiAVObject::world (NiLayout::kWorldOffset).
glm::vec3 WorldPositionOf(TESObjectREFR* apRef) noexcept
{
    if (NiNode* pNode = apRef->GetNiNode())
    {
        const auto& world = WorldTransformOf(pNode);
        return {world.translate.x, world.translate.y, world.translate.z};
    }
    return {apRef->position.x, apRef->position.y, apRef->position.z};
}

/**
 * Find how an item's 3D is turned in the world, which follows physics like WorldPositionOf.
 *
 * @param apNode The item's 3D.
 * @return Its rotation in the game's world.
 */
glm::mat3 WorldTurnOf(const NiNode* apNode) noexcept
{
    const auto& world = WorldTransformOf(apNode);
    return ToGlm(world.rotate);
}

//! How an item is really turned, as the reference's angles: like its position, the reference's own angles
//! lag its 3D, and stay as they were the whole time a hand holds it.
glm::vec3 RotationOf(TESObjectREFR* apRef) noexcept
{
    if (const NiNode* pNode = apRef->GetNiNode())
        return ReferenceAngles::FromTurn(WorldTurnOf(pNode));
    return {apRef->rotation.x, apRef->rotation.y, apRef->rotation.z};
}

//! The angle in radians of the turn from one rotation to another.
float AngleBetween(const glm::mat3& acFrom, const glm::mat3& acTo) noexcept
{
    // The trace of the relative rotation is 1 + 2 cos(angle).
    float trace = 0.f;
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            trace += acFrom[row][column] * acTo[row][column];
    return std::acos(std::clamp((trace - 1.f) * 0.5f, -1.f, 1.f));
}

double Milliseconds(const std::chrono::steady_clock::time_point aTime) noexcept
{
    return std::chrono::duration<double, std::milli>(aTime.time_since_epoch()).count();
}

//! A 3D's world place and turn, read as WorldPositionOf and WorldTurnOf do.
HeldObject::Pose WorldPoseOf(const NiNode* apNode) noexcept
{
    const auto& world = WorldTransformOf(apNode);
    return {{world.translate.x, world.translate.y, world.translate.z}, glm::normalize(glm::quat_cast(WorldTurnOf(apNode)))};
}

NiNode* FindNamed(NiNode* apRoot, const BSFixedString& acName) noexcept
{
    using TGetObjectByName = void*(void*, const char* const*, char);
    POINTER_SKYRIMSE(TGetObjectByName, GetObjectByName, 76207, 76207);
    if (!apRoot)
        return nullptr;
    return static_cast<NiNode*>(GetObjectByName(apRoot, &acName.data, 1));
}

// The game's ExtraDroppedItemList, as CommonLibSSE has it: the BSExtraData header, then a BSSimpleList of handles.
// Only read after RTTI confirms the type, and every node is checked readable, so a wrong layout reads nothing.
struct DroppedItemNode
{
    uint32_t Handle;
    uint32_t Pad;
    DroppedItemNode* Next;
};
struct DroppedItemListView
{
    void* VTable;
    void* NextExtra;
    DroppedItemNode Head;
};

// The extra data of the player's stack the placed item is dropped out of again: the list handed to the game, while the inventory still holds it.
// Otherwise an equal list the item went into, or none; a list the game no longer holds is never handed back to it.
ExtraDataList* StackListOf(TESObjectREFR* apHolder, const TESForm* apObject, ExtraDataList* apGiven, const Inventory::Entry& acEntry) noexcept
{
    const ExtraContainerChanges::Data* pChanges = apHolder->GetContainerChanges();
    if (!apGiven || !pChanges || !pChanges->entries)
        return nullptr;

    ExtraDataList* pEqual = nullptr;
    for (ExtraContainerChanges::Entry* pEntry : *pChanges->entries)
    {
        if (!pEntry || pEntry->form != apObject || !pEntry->dataList)
            continue;
        for (ExtraDataList* pList : *pEntry->dataList)
        {
            if (!pList)
                continue;
            if (pList == apGiven)
                return apGiven;
            if (!pEqual)
            {
                Inventory::Entry held{};
                held.BaseId = acEntry.BaseId;
                held.Count = 1;
                TESObjectREFR::GetItemFromExtraData(held, pList);
                if (held.IsExtraDataEquals(acEntry))
                    pEqual = pList;
            }
        }
    }
    return pEqual;
}

//! A move sent while a hand of this player's let go of the object and it flies by itself carries how it flies.
template <class T> void AddThrow(T& aRequest, const HeldObject::Release& acRelease, const HeldObject::Flight& acFlight) noexcept
{
    if (aRequest.AtRest || !acRelease.IsThrown())
        return;
    aRequest.Thrown = true;
    aRequest.Velocity = acFlight.Velocity;
    aRequest.Spin = acFlight.Spin;
}
} // namespace

DroppedItemService::DroppedItemService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_connectedConnection = aDispatcher.sink<ConnectedEvent>().connect<&DroppedItemService::OnConnected>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&DroppedItemService::OnDisconnected>(this);
    m_cellChangeConnection = aDispatcher.sink<CellChangeEvent>().connect<&DroppedItemService::OnCellChange>(this);
    m_droppedConnection = aDispatcher.sink<ItemDroppedEvent>().connect<&DroppedItemService::OnItemDropped>(this);
    m_pickedUpConnection = aDispatcher.sink<ItemPickedUpEvent>().connect<&DroppedItemService::OnItemPickedUp>(this);
    m_notifyConnection = aDispatcher.sink<NotifyDroppedItem>().connect<&DroppedItemService::OnNotifyDroppedItem>(this);
    m_removedConnection = aDispatcher.sink<NotifyDroppedItemRemoved>().connect<&DroppedItemService::OnNotifyDroppedItemRemoved>(this);
    m_moveConnection = aDispatcher.sink<NotifyDroppedItemMove>().connect<&DroppedItemService::OnNotifyDroppedItemMove>(this);
    m_objectMoveConnection = aDispatcher.sink<NotifyWorldObjectMove>().connect<&DroppedItemService::OnNotifyWorldObjectMove>(this);
    m_propBodiesConnection = aDispatcher.sink<NotifyPropBodies>().connect<&DroppedItemService::OnNotifyPropBodies>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&DroppedItemService::OnUpdate>(this);
}

void DroppedItemService::OnConnected(const ConnectedEvent&) noexcept
{
    AnnounceOldDrops();
}

void DroppedItemService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    // A dropped item the other player was holding is let go here, from where they had it.
    const auto now = std::chrono::steady_clock::now();
    for (auto& [id, motion] : m_motion)
    {
        if (!motion.Held)
            continue;
        const auto it = m_refById.find(id);
        TESObjectREFR* pRef = it != m_refById.end() ? RefOf(it->second) : nullptr;
        if (pRef && IsStillOurs(pRef, motion) && !pRef->IsDeleted())
        {
            PutAtNewest(pRef, motion, now);
            if (!motion.RemoteFixed)
                pRef->SetMotionType(TESObjectREFR::kMotionDynamic);
        }
    }

    // Ids belong to one server session's view; a reconnect gets them again with the next cell entry.
    m_refById.clear();
    m_idByRef.clear();
    m_pending.clear();
    m_awaitingItems.clear();
    m_deferredItems.clear();
    m_removingItems.clear();
    m_retryDeferred = false;
    m_announced.clear();
    m_reportedOldDrops = false;
    m_motion.clear();

    // Whatever the other player was holding is let go here.
    for (const auto& [formId, motion] : m_objectMotion)
    {
        if (!motion.Held)
            continue;
        if (TESObjectREFR* pRef = Cast<TESObjectREFR>(TESForm::GetById(formId)); pRef && !pRef->IsDeleted())
            pRef->SetMotionType(TESObjectREFR::kMotionDynamic);
    }
    m_objectMotion.clear();
    m_objectCandidates.clear();
    PropHavok::ClearSamples();
}

void DroppedItemService::OnCellChange(const CellChangeEvent&) noexcept
{
    PropHavok::ClearSamples();
    if (m_transport.IsConnected())
    {
        m_world.GetRunner().Queue(
            [this]
            {
                if (!m_transport.IsConnected())
                    return;
                for (const auto& message : DroppedItemReplies::Replays(m_awaitingItems))
                    OnNotifyDroppedItem(message);
            });
        AnnounceOldDrops();
    }
}

void DroppedItemService::MapServerIdToRef(const uint32_t aId, const uint32_t aRefFormId) noexcept
{
    std::erase_if(m_awaitingItems, [aId](const auto& acWaiting) { return acWaiting.ServerId == aId; });
    m_refById[aId] = aRefFormId;
    m_idByRef[aRefFormId] = aId;

    // A placed or adopted item needs time to settle before its movement is sent.
    // A new drop's owner clears this delay once the server returns its id, so its initial flight is sent too.
    Motion& motion = m_motion[aId];
    motion = Motion{};
    if (TESObjectREFR* pRef = RefOf(aRefFormId))
    {
        motion.LastPosition = WorldPositionOf(pRef);
        motion.LastRotation = RotationOf(pRef);
        motion.BaseFormId = pRef->baseForm ? pRef->baseForm->formID : 0;
        motion.RefHandle = pRef->GetHandle().handle.iBits;
        motion.Loaded = pRef->GetNiNode() != nullptr;
    }
    motion.QuietUntil = std::chrono::steady_clock::now() + std::chrono::seconds(2);
}

bool DroppedItemService::SendAdd(TESObjectREFR* apRef, const Inventory::Entry& acItem, const bool aAnnouncement) noexcept
{
    // A run-time item no other game can make stays this game's own.
    if (!acItem.BaseId || !ItemDataChanges::IsSyncable(acItem))
        return false;

    auto& modSystem = m_world.GetModSystem();

    RequestDroppedItemAdd request{};
    request.Item = acItem;
    request.Announcement = aAnnouncement;
    if (++m_addRequests == 0)
        ++m_addRequests;
    request.RequestId = m_addRequests;
    request.Position = glm::vec3(apRef->position.x, apRef->position.y, apRef->position.z);
    // As its 3D is turned, like its moves (RotationOf): the reference's own angles lag behind it.
    request.Rotation = RotationOf(apRef);

    TESObjectCELL* pCell = apRef->GetParentCellEx();
    if (!pCell || !modSystem.GetServerModId(pCell->formID, request.CellId))
    {
        spdlog::warn("DroppedItem: cannot name the cell item {:X} lies in, so it is not shared", apRef->formID);
        return false;
    }
    if (TESWorldSpace* pWorldSpace = apRef->GetWorldSpace())
        modSystem.GetServerModId(pWorldSpace->formID, request.WorldSpaceId);

    m_transport.Send(request);
    m_pending.push_back(Pending{apRef->formID, apRef->GetHandle().handle.iBits, request.RequestId, acItem, aAnnouncement, request.CellId, request.WorldSpaceId, request.Position});
    return true;
}

void DroppedItemService::OnItemDropped(const ItemDroppedEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
    {
        // Kept for the next connection (see m_droppedOffline).
        if (acEvent.Item.BaseId != GameId{} && m_droppedOffline.size() < 256)
        {
            m_droppedOffline.emplace_back(acEvent.RefFormId, acEvent.Item);
            spdlog::info("DroppedItem: dropped {:X} while not connected; it is announced once connected", acEvent.RefFormId);
        }
        return;
    }

    TESObjectREFR* pRef = Cast<TESObjectREFR>(TESForm::GetById(acEvent.RefFormId));
    if (!pRef || acEvent.Item.BaseId == GameId{})
        return;

    if (SendAdd(pRef, acEvent.Item, false))
        spdlog::info("DroppedItem: dropped {:X}:{:X} x{} as {:X}, shared with the server", acEvent.Item.BaseId.ModId, acEvent.Item.BaseId.BaseId,
                     std::abs(acEvent.Item.Count), pRef->formID);
}

void DroppedItemService::OnItemPickedUp(const ItemPickedUpEvent& acEvent) noexcept
{
    const auto it = m_idByRef.find(acEvent.RefFormId);
    if (it == m_idByRef.end())
    {
        if (DroppedItemReplies::PickedUpScoped(
                m_pending, m_awaitingItems, acEvent.RefFormId, acEvent.RefHandle, acEvent.Item, acEvent.CellId, acEvent.WorldSpaceId, acEvent.Position, kSameItemRadius,
                [](uint32_t aHandle) { return TESObjectREFR::GetByHandle(aHandle) != nullptr; }, [&](uint32_t aId) { RemovePickedUpItem(aId, acEvent.Item); }))
            return;
        if (acEvent.Placed && m_transport.IsConnected())
        {
            m_world.GetDispatcher().trigger(ItemTakeEvent{ItemTakeSource::kPlacedItem, 0, acEvent.ObjectId, acEvent.CellId, acEvent.Item, acEvent.WorldSpaceId, acEvent.Position});
            m_objectMotion.erase(acEvent.RefFormId);
        }
        return; // not an item the server was keeping
    }

    RemovePickedUpItem(it->second, acEvent.Item);

    spdlog::info("DroppedItem: picked up {} ({:X}); asking everyone to take it off the floor", it->second, acEvent.RefFormId);
    m_motion.erase(it->second);
    m_refById.erase(it->second);
    m_idByRef.erase(it);
}

void DroppedItemService::RemovePickedUpItem(uint32_t aId, const Inventory::Entry& acItem) noexcept
{
    if (!m_removingItems.insert(aId).second)
        return;
    m_deferredItems.erase(aId);
    RequestDroppedItemRemove request{};
    request.Id = aId;
    m_transport.Send(request);
    m_world.GetDispatcher().trigger(ItemTakeEvent{ItemTakeSource::kDroppedItem, aId, {}, {}, acItem});
}

void DroppedItemService::OnNotifyDroppedItem(const NotifyDroppedItem& acIncoming) noexcept
{
    NotifyDroppedItem message = acIncoming;
    const auto& acMessage = message;
    if (m_removingItems.contains(acMessage.Id))
        return;
    if (!ClientTrust::IsUsableTransform(acMessage.Position.x, acMessage.Position.y, acMessage.Position.z, acMessage.Rotation.x, acMessage.Rotation.y, acMessage.Rotation.z))
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    auto& modSystem = m_world.GetModSystem();
    const glm::vec3 cPosition(acMessage.Position);

    const auto isLoaded = [&]
    {
        if (!pPlayer)
            return false;
        if (!acMessage.WorldSpaceId)
        {
            TESObjectCELL* pCell = pPlayer->GetParentCellEx();
            GameId cellId{};
            return pCell && modSystem.GetServerModId(pCell->formID, cellId) && cellId == acMessage.CellId;
        }
        TESWorldSpace* pWorldSpace = pPlayer->GetWorldSpace();
        GameId worldSpaceId{};
        return pWorldSpace && modSystem.GetServerModId(pWorldSpace->formID, worldSpaceId) && worldSpaceId == acMessage.WorldSpaceId &&
               DistanceTo(pPlayer, cPosition) <= kLoadedRadius;
    };

    if (const auto mapped = m_refById.find(acMessage.Id); mapped != m_refById.end())
    {
        // Already here: the server resends it on every cell change, with where it was last left.
        if (std::erase_if(
                m_pending, [&](const Pending& acPending)
                { return acPending.RefFormId == mapped->second && DroppedItemIdentity::IsReply(acPending.RequestId, acPending.Item, acMessage.RequestId, acMessage.Item); }))
            m_retryDeferred = true;
        if (!isLoaded())
            return;
        TESObjectREFR* pRef = RefOf(mapped->second);
        Motion& motion = m_motion[acMessage.Id];
        Inventory::Entry onFloor{};
        const bool cLive =
            pRef && IsStillOurs(pRef, motion) && !pRef->IsDeleted() && !pRef->IsDisabled() && ReadFloorItem(pRef, onFloor) &&
            DroppedItemReplies::MappedMatches(motion.RefHandle, pRef->GetHandle().handle.iBits, acMessage.Item, onFloor, motion.LegacyItemData || acMessage.LegacyItemData);
        if (!cLive)
        {
            m_idByRef.erase(mapped->second);
            m_refById.erase(mapped);
            m_motion.erase(acMessage.Id);
            message.Replay = true;
        }
        else
        {
            if (motion.LegacyItemData && !acMessage.LegacyItemData && pRef && IsStillOurs(pRef, motion) && !pRef->IsDeleted())
            {
                ItemExtraData::Update(&pRef->extraData, acMessage.Item);
                motion.LegacyItemData = false;
            }
            const auto now = std::chrono::steady_clock::now();
            const glm::vec3 cLeftAt(acMessage.Position);
            // Held still where it was left (a weapon rack's weapon), or no longer: kept out of physics only while it is.
            // Not this game's own drop, which its own script holds, nor while this player moves it.
            const bool cIdle = !motion.OwnDrop && !motion.Held && !motion.Moving && now - motion.LastLocalMove > std::chrono::seconds(2);
            if (cIdle && acMessage.Fixed)
            {
                HoldStillWhenFixed(motion, acMessage);
                return;
            }
            if (cIdle && motion.RemoteFixed && pRef && IsStillOurs(pRef, motion) && !pRef->IsDeleted())
            {
                motion.RemoteFixed = false;
                motion.FixPending = false;
                pRef->SetMotionType(TESObjectREFR::kMotionDynamic);
            }
            if (pRef && IsStillOurs(pRef, motion) && !pRef->IsDeleted() && !motion.Held && !motion.Moving && now - motion.LastLocalMove > std::chrono::seconds(2) &&
                DistanceTo(pRef, cLeftAt) > 20.f)
            {
                spdlog::info(
                    "DroppedItem: {} ({:X}) was moved while we were away; putting it where it was left, {:.0f} units off", acMessage.Id, mapped->second, DistanceTo(pRef, cLeftAt));
                pRef->SetRotation(acMessage.Rotation.x, acMessage.Rotation.y, acMessage.Rotation.z);
                if (TESObjectCELL* pCell = pRef->GetParentCellEx())
                    pRef->MoveTo(pCell, NiPoint3(cLeftAt));
                motion.LastPosition = cLeftAt;
                motion.LastRotation = acMessage.Rotation;
                motion.QuietUntil = now + std::chrono::seconds(2);
            }
            return;
        }
    }

    const auto result = DroppedItemReplies::Receive(
        m_pending, m_awaitingItems, message,
        [&](const Pending& acPending)
        {
            TESObjectREFR* pRef = TESObjectREFR::GetByHandle(acPending.RefHandle);
            Inventory::Entry onFloor{};
            if (!pRef || pRef->formID != acPending.RefFormId || !ReadFloorItem(pRef, onFloor) || !DroppedItemIdentity::SameStack(onFloor, acMessage.Item))
                return false;
            MapServerIdToRef(acMessage.Id, acPending.RefFormId);
            Motion& motion = m_motion[acMessage.Id];
            motion.LegacyItemData = acMessage.LegacyItemData;
            // This save's copy, announced where it rested when this player left, matched an item moved far since: it goes there.
            if (acPending.Announcement && DistanceTo(pRef, cPosition) > kSameItemRadius)
            {
                spdlog::info(
                    "DroppedItem: {} ({:X}) was moved while this player was away; putting it where it lies now, {:.0f} units off", acMessage.Id, acPending.RefFormId,
                    DistanceTo(pRef, cPosition));
                PlaceMovedObject(pRef, cPosition, acMessage.Rotation);
                const auto now = std::chrono::steady_clock::now();
                motion.LastPosition = cPosition;
                motion.LastRotation = acMessage.Rotation;
                motion.QuietUntil = now + std::chrono::seconds(2);
            }
            if (!acPending.Announcement)
            {
                const auto now = std::chrono::steady_clock::now();
                motion.QuietUntil = now;
                motion.FreshDropUntil = now + std::chrono::seconds(2);
                // Only a fresh drop is this game's own: an announcement can be a copy placed here for another player, held still by its owner's script.
                motion.OwnDrop = true;
            }
            spdlog::debug("DroppedItem: {} is our own {:X}", acMessage.Id, acPending.RefFormId);
            return true;
        },
        [&](uint32_t aId) { RemovePickedUpItem(aId, acMessage.Item); }, isLoaded);
    if (result.Consumed)
        m_retryDeferred = true;
    if (result.Action != DroppedItemReplies::Outcome::Place)
        return;

    // A made potion or poison is made here first.
    TESBoundObject* pBase = ItemExtraData::GetBase(acMessage.Item);
    const uint32_t cBaseFormId = pBase ? pBase->formID : 0;

    // 2. Already lying here: kept by this save from an earlier session, or placed before a reconnect.
    // A replay near an in-flight add waits for its request-correlated reply: near where the item lies now, or near one of
    // its earlier resting places, where this save's copy, announced and not yet answered, may lie.
    const auto cWaitingNear = [&](const Pending& acPending, const glm::vec3& acPlace)
    {
        return DroppedItemIdentity::WaitingForReplay(
            acPending, acMessage.Item, acMessage.Replay, acMessage.LegacyItemData, acMessage.CellId, acMessage.WorldSpaceId, acPlace, kSameItemRadius);
    };
    if (std::any_of(
            m_pending.begin(), m_pending.end(),
            [&](const Pending& acPending)
            {
                return cWaitingNear(acPending, glm::vec3(acMessage.Position)) ||
                       std::any_of(
                           acMessage.EarlierPlaces.begin(), acMessage.EarlierPlaces.end(),
                           [&](const Vector3_NetQuantize& acPlace) { return cWaitingNear(acPending, glm::vec3(acPlace)); });
            }))
    {
        if (m_deferredItems.size() < 256 || m_deferredItems.contains(acMessage.Id))
            m_deferredItems.insert_or_assign(acMessage.Id, acMessage);
        return;
    }
    // This save's copy near acCenter: in the item's cell and the player's.
    const auto findNear = [&](const glm::vec3& acCenter, const bool aRequireUnique)
    {
        DroppedItemIdentity::Nearby nearby(acMessage.Item, acMessage.LegacyItemData, kSameItemRadius, acMessage.Replay, aRequireUnique);
        const auto scan = [&](const TESObjectCELL* apCell)
        {
            if (!apCell || !acMessage.Replay)
                return;
            for (TESObjectREFR* pRef : apCell->GetRefsByFormTypes(kItemTypes))
            {
                if (!pRef || !pRef->baseForm || pRef->baseForm->formID != cBaseFormId || pRef->IsDisabled() || pRef->IsDeleted() || m_idByRef.count(pRef->formID))
                    continue;
                const float distance = DistanceTo(pRef, acCenter);
                Inventory::Entry onFloor{};
                if (distance <= kSameItemRadius && ReadFloorItem(pRef, onFloor))
                {
                    const bool cReserved = DroppedItemReplies::Reserved(m_pending, m_awaitingItems, pRef->formID, acMessage.Id);
                    nearby.Consider(pRef->formID, onFloor, distance, cReserved, pRef->IsTemporary());
                }
            }
        };
        scan(Cast<TESObjectCELL>(TESForm::GetById(modSystem.GetGameId(acMessage.CellId))));
        scan(pPlayer->GetParentCellEx());
        return nearby.Get();
    };

    auto cNearby = findNear(cPosition, false);
    if (cNearby.Ambiguous)
        return;
    // Not where it lies now: this save may still have it where it rested before somebody moved it while this player was away.
    // Found there, it is the item, moved to where it lies now; nothing is placed a second time.
    bool movedFromEarlier = false;
    for (size_t i = 0; !cNearby.Id && acMessage.Replay && i < acMessage.EarlierPlaces.size(); ++i)
    {
        const glm::vec3 cEarlier(acMessage.EarlierPlaces[i]);
        if (!ClientTrust::IsUsableTransform(cEarlier.x, cEarlier.y, cEarlier.z, 0.f, 0.f, 0.f) || glm::distance(cEarlier, cPosition) <= kSameItemRadius)
            continue;
        // Only a single matching copy there is taken: two or more, and none is moved (the wrong stack must not be).
        cNearby = findNear(cEarlier, true);
        if (cNearby.Ambiguous)
        {
            spdlog::info(
                "DroppedItem: {} may lie where it rested before, {:.0f} units from where it lies now, but more than one matching copy is there; "
                "none is moved and none placed",
                acMessage.Id, glm::distance(cEarlier, cPosition));
            return;
        }
        movedFromEarlier = cNearby.Id != 0;
    }
    if (TESObjectREFR* pFound = cNearby.Id ? Cast<TESObjectREFR>(TESForm::GetById(cNearby.Id)) : nullptr)
    {
        if (movedFromEarlier)
        {
            spdlog::info(
                "DroppedItem: {} was moved while this player was away; this save's copy {:X} found where it rested before, {:.0f} units off, and moved "
                "to where it lies now",
                acMessage.Id, pFound->formID, DistanceTo(pFound, cPosition));
            PlaceMovedObject(pFound, cPosition, acMessage.Rotation);
        }
        MapServerIdToRef(acMessage.Id, pFound->formID);
        m_motion[acMessage.Id].LegacyItemData = acMessage.LegacyItemData;
        if (movedFromEarlier)
        {
            // Put there by the server's word, not moved by this player: not sent back as a move.
            Motion& motion = m_motion[acMessage.Id];
            motion.LastPosition = cPosition;
            motion.LastRotation = acMessage.Rotation;
            motion.QuietUntil = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }
        HoldStillWhenFixed(m_motion[acMessage.Id], acMessage);
        if (acMessage.LegacyItemData)
        {
            Inventory::Entry onFloor{};
            if (ReadFloorItem(pFound, onFloor))
                SendAdd(pFound, onFloor, true);
        }
        spdlog::info("DroppedItem: {} was already lying here as {:X}, {:.0f} units from where it was dropped", acMessage.Id, pFound->formID, DistanceTo(pFound, cPosition));
        return;
    }

    // 3. Missing: give it to the player and drop it straight back out at the item's position.
    // Both run under the inventory override, so neither is sent as a change or as this player's drop.
    TESBoundObject* pObject = pBase;
    if (!pObject)
    {
        spdlog::warn("DroppedItem: {} is {:X}:{:X}, which this game does not have", acMessage.Id, acMessage.Item.BaseId.ModId, acMessage.Item.BaseId.BaseId);
        return;
    }

    Inventory::Entry entry = acMessage.Item;
    const int32_t cCount = std::max(1, std::abs(entry.Count));
    entry.Count = cCount;

    BSPointerHandle<TESObjectREFR> handle{};
    {
        ScopedInventoryOverride _;
        ExtraDataList* pExtra = TESObjectREFR::GetExtraDataFromItem(entry);
        NiPoint3 location(cPosition);
        NiPoint3 rotation(acMessage.Rotation);
        pPlayer->AddObjectToContainer(pObject, pExtra, cCount, nullptr);
        ExtraDataList* pStack = StackListOf(pPlayer, pObject, pExtra, entry);
        if (pExtra && pStack != pExtra)
            spdlog::warn("DroppedItem: {} ({:X}): the game did not keep the item's extra data as given; dropped from {}", acMessage.Id, pObject->formID,
                         pStack ? "an equal stack" : "a plain stack");
        handle = pPlayer->RemoveItem(pObject, cCount, ITEM_REMOVE_REASON::kDropping, pStack, nullptr, &location, &rotation);
    }

    if (TESObjectREFR* pPlaced = TESObjectREFR::GetByHandle(handle.handle.iBits))
    {
        MapServerIdToRef(acMessage.Id, pPlaced->formID);
        m_motion[acMessage.Id].LegacyItemData = acMessage.LegacyItemData;
        HoldStillWhenFixed(m_motion[acMessage.Id], acMessage);
        spdlog::info("DroppedItem: placed {} ({:X} x{}) as {:X}", acMessage.Id, pObject->formID, cCount, pPlaced->formID);
    }
    else
        spdlog::warn("DroppedItem: placing {} ({:X}) produced no reference", acMessage.Id, pObject->formID);
}

void DroppedItemService::OnNotifyDroppedItemRemoved(const NotifyDroppedItemRemoved& acMessage) noexcept
{
    m_removingItems.erase(acMessage.Id);
    std::erase_if(m_awaitingItems, [&](const auto& acWaiting) { return acWaiting.ServerId == acMessage.Id; });
    m_deferredItems.erase(acMessage.Id);
    const auto it = m_refById.find(acMessage.Id);
    if (it == m_refById.end())
        return; // not here, or picked up here

    const uint32_t cRefFormId = it->second;
    m_idByRef.erase(cRefFormId);
    m_refById.erase(it);
    uint32_t mappedBase = 0;
    uint32_t mappedHandle = 0;
    if (const auto cMotion = m_motion.find(acMessage.Id); cMotion != m_motion.end())
    {
        mappedBase = cMotion->second.BaseFormId;
        mappedHandle = cMotion->second.RefHandle;
        m_motion.erase(cMotion);
    }

    TESObjectREFR* pRef = Cast<TESObjectREFR>(TESForm::GetById(cRefFormId));
    if (!pRef || pRef->IsDeleted())
        return;

    // The item may have gone here without passing a pick-up (a cell reset, a script, an NPC took it), and the game given its
    // id to another object since: that one is left alone.
    const uint32_t cLiveBase = pRef->baseForm ? pRef->baseForm->formID : 0;
    if (!DroppedItemReplies::StillTheItem(mappedBase, mappedHandle, cLiveBase, pRef->GetHandle().handle.iBits, Cast<Actor>(pRef) != nullptr))
    {
        spdlog::info("DroppedItem: {} was picked up elsewhere; {:X} here is another object now (base {:X}, was {:X}), left alone", acMessage.Id, cRefFormId, cLiveBase, mappedBase);
        return;
    }

    // Disabled, then Papyrus Delete, so the game frees it once nothing holds it.
    pRef->Disable();
    pRef->Delete();
    spdlog::info("DroppedItem: {} was picked up elsewhere; removed {:X} here", acMessage.Id, cRefFormId);
}

void DroppedItemService::AnnounceOldDrops() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    // First what this client saw the player drop while offline, if it still lies where it fell.
    if (!m_droppedOffline.empty() && m_transport.IsConnected())
    {
        uint32_t announced = 0;
        for (const auto& [refFormId, item] : m_droppedOffline)
        {
            TESObjectREFR* pRef = Cast<TESObjectREFR>(TESForm::GetById(refFormId));
            Inventory::Entry onFloor{};
            if (m_idByRef.count(refFormId) || m_announced.count(refFormId) || DroppedItemReplies::Reserved(m_pending, m_awaitingItems, refFormId) ||
                !ReadFloorItem(pRef, onFloor) || !DroppedItemIdentity::SameStack(item, onFloor))
                continue;
            m_announced.insert(refFormId);
            if (SendAdd(pRef, onFloor, true))
                ++announced;
        }
        spdlog::info("DroppedItem: {} of the {} item(s) dropped while not connected announced", announced, m_droppedOffline.size());
        m_droppedOffline.clear();
    }

    // Both types have to have resolved on this build: DynamicCast with a null type descriptor is not safe to call.
    if (!internal::RttiLocator<BSExtraData>::Get() || !internal::RttiLocator<ExtraDroppedItemList>::Get())
    {
        static bool s_said = false;
        if (!s_said)
        {
            s_said = true;
            spdlog::warn("DroppedItem: the dropped-item list type did not resolve on this build; earlier drops are not announced");
        }
        return;
    }

    uint32_t found = 0;
    uint32_t sent = 0;

    uint32_t guard = 0;
    bool listFound = false;
    for (BSExtraData* pExtra = pPlayer->extraData.data; pExtra && guard < 512; pExtra = pExtra->next, ++guard)
    {
        if (!ReadableMemory::IsReadable(pExtra, sizeof(BSExtraData)) || !Cast<ExtraDroppedItemList>(pExtra))
            continue;
        listFound = true;

        const auto* pList = reinterpret_cast<const DroppedItemListView*>(pExtra);
        const DroppedItemNode* pNode = &pList->Head;
        for (uint32_t n = 0; pNode && n < 1024 && ReadableMemory::IsReadable(pNode, sizeof(DroppedItemNode)); ++n, pNode = pNode->Next)
        {
            if (!pNode->Handle)
                continue;

            TESObjectREFR* pRef = TESObjectREFR::GetByHandle(pNode->Handle);
            if (!IsItem(pRef) || pRef->IsDisabled() || pRef->IsDeleted())
                continue;
            ++found;

            if (m_idByRef.count(pRef->formID) || m_announced.count(pRef->formID) || DroppedItemReplies::Reserved(m_pending, m_awaitingItems, pRef->formID))
                continue;
            m_announced.insert(pRef->formID);

            Inventory::Entry item{};
            if (!ReadFloorItem(pRef, item))
                continue;

            if (SendAdd(pRef, item, true))
                ++sent;
        }
        break; // one list per actor
    }

    // Logged once per connection even when nothing is found.
    if (found || sent || !m_reportedOldDrops)
    {
        m_reportedOldDrops = true;
        if (listFound)
            spdlog::info("DroppedItem: the player's dropped-item list holds {} item(s); announced {} not yet shared this session", found, sent);
    }
}

void DroppedItemService::OnUpdate(const UpdateEvent&) noexcept
{
    PerfScope perfScope("DroppedItemService::OnUpdate");

    if (!m_transport.IsConnected())
        return;

    if (m_retryDeferred)
    {
        m_retryDeferred = false;
        auto deferred = std::move(m_deferredItems);
        m_deferredItems.clear();
        for (const auto& [id, item] : deferred)
            OnNotifyDroppedItem(item);
    }

    // Thirty times a second: the receiver snaps to each update, so fewer would stutter.
    // On a grid with some slack (SendGrid.h), as the player's own movement is: a frame a little early still sends.
    static SendGrid::Schedule s_sends{SendGrid::cMovementInterval, SendGrid::cMovementSlack};
    const auto now = std::chrono::steady_clock::now();
    ShowHeldObjects(now);
    DrivePropBodies();
    if (!s_sends.IsDue(now))
        return;

    ReadHolds();
    std::vector<ItemSend> itemSends;
    for (const auto& [id, refFormId] : m_refById)
    {
        Motion& motion = m_motion[id];
        TESObjectREFR* pRef = RefOf(refFormId);
        if (!pRef || !IsStillOurs(pRef, motion) || pRef->IsDeleted() || pRef->IsDisabled() || !pRef->GetNiNode())
        {
            motion.Loaded = false;
            continue;
        }

        const glm::vec3 position = WorldPositionOf(pRef);
        const glm::vec3 rotation = RotationOf(pRef);
        const glm::mat3 turn = WorldTurnOf(pRef->GetNiNode());

        // Its cell just loaded: its settling is ignored, so neither side sends it.
        if (!motion.Loaded)
        {
            motion.Loaded = true;
            if (now >= motion.FreshDropUntil)
                motion.QuietUntil = std::max(motion.QuietUntil, now + std::chrono::seconds(2));
            // A new 3D has a dynamic body again: a weapon on a rack would fall off.
            motion.FixPending = motion.FixPending || motion.RemoteFixed;
        }
        const bool cHeldHere = PollHeld(pRef, motion, position);
        if (motion.Caught.Poll(motion.Held && motion.RemoteThrow.IsFlying(), cHeldHere))
            CatchHere(pRef, motion, now, "DroppedItem", id);
        if (motion.FixPending)
        {
            HoldFixed(pRef, motion);
            continue;
        }

        // Nothing heard for two seconds from the side moving it: back to physics here.
        if (motion.Held && now - motion.LastRemote > std::chrono::seconds(2))
        {
            PutAtNewest(pRef, motion, now);
            if (!motion.RemoteFixed)
                pRef->SetMotionType(TESObjectREFR::kMotionDynamic);
            motion.Held = false;
            motion.QuietUntil = now + std::chrono::seconds(2);
            motion.StopShowing();
            motion.RemoteThrow = {};
            if (motion.RemoteFixed)
                spdlog::debug("DroppedItem: {} was put down over there and is held still there by a script; kept still here", id);
            else if (motion.RemoteAtRest)
                spdlog::info("DroppedItem: {} was put down over there; handed back to physics here", id);
            else
                spdlog::info("DroppedItem: {} stopped being moved over there without coming to rest; released it here", id);
        }

        if (motion.Held || now < motion.QuietUntil)
        {
            motion.LastPosition = position;
            motion.LastRotation = rotation;
            motion.LastTurn = turn;
            motion.OwnRelease.Rest();
            continue;
        }

        const bool cMoved = glm::distance(position, motion.LastPosition) > kMovedDistance || AngleBetween(motion.LastTurn, turn) > kMovedAngle;
        // An item a script holds still (a weapon rack's) is moved by it, never thrown: its teleport is no flight.
        const bool cFree = (cMoved || motion.Moving) && (motion.WasRemoteFixed || !IsHeldStill(pRef));
        NoteFlight(pRef, motion, position, turn, Milliseconds(now), cHeldHere, cFree);

        // Moved here (a hand took it off its rack): no longer held still for another game, and physics has it again.
        if (cMoved && motion.RemoteFixed)
        {
            pRef->SetMotionType(TESObjectREFR::kMotionDynamic);
            motion.RemoteFixed = false;
            motion.FixPending = false;
        }

        if (cMoved && (!motion.Moving || motion.Blast))
        {
            // Sent with the placed objects' starts, which tell a blast from a hand; a move is measured from the last place sent until then.
            if (motion.Moving)
                motion.LastLocalMove = now;
            itemSends.push_back({id, refFormId, position, rotation, turn, !motion.Moving});
        }
        else if (cMoved)
        {
            motion.LastPosition = position;
            motion.LastRotation = rotation;
            motion.LastTurn = turn;
            motion.LastLocalMove = now;
            SendItemMove(id, position, rotation, false, false);
            m_moveBudget.Spend(now, 1);
        }
        else if (motion.Moving && now - motion.LastLocalMove >= std::chrono::milliseconds(600))
        {
            // Still for long enough to call it put down. The server keeps this place for anyone who comes later.
            motion.Moving = false;
            motion.Blast = false;
            motion.OwnRelease.Rest();
            const bool cHeldStill = !motion.WasRemoteFixed && IsHeldStill(pRef);
            SendItemMove(id, position, rotation, true, false, cHeldStill);
            spdlog::debug("DroppedItem: {} came to rest at ({:.0f}){}", id, position, cHeldStill ? ", held still there by a script" : "");
        }
    }

    UpdateWorldObjects(now, itemSends);
}

// A move in between may be lost, as the next follows a frame later; where it starts and where it rests always arrive.
void DroppedItemService::SendItemMove(
    const uint32_t aId, const glm::vec3& acPosition, const glm::vec3& acRotation, const bool aAtRest, const bool aStart, const bool aFixed) noexcept
{
    RequestDroppedItemMove request{};
    request.Id = aId;
    request.Position = acPosition;
    request.Rotation = acRotation;
    request.AtRest = aAtRest;
    request.Fixed = SharedHomes::KeptStill(aAtRest, aFixed);
    request.Tick = m_transport.GetClock().GetCurrentTick();
    // One held still where it was put down never flies on (SharedHomes::KeptStill).
    if (const auto found = m_motion.find(aId); found != m_motion.end() && !request.Fixed)
        AddThrow(request, found->second.OwnRelease, found->second.OwnFlight);
    m_transport.Send(request, aAtRest || aStart ? TiltedPhoques::kReliable : TiltedPhoques::kUnreliable);
}

namespace
{
// What a hand can pick up or push: items, alchemy apparatus (33) and movable statics (36: carts, hanging buckets).
// Statics and furniture never move.
bool IsMovableObject(const TESObjectREFR* apRef) noexcept
{
    if (IsItem(apRef))
        return true;
    if (!apRef || !apRef->baseForm)
        return false;
    const auto cType = static_cast<uint8_t>(apRef->baseForm->formType);
    return cType == 33 || cType == 36;
}
} // namespace

TESObjectREFR* DroppedItemService::RefOf(const uint32_t aRefFormId) const noexcept
{
    return Cast<TESObjectREFR>(TESForm::GetById(aRefFormId));
}

// Objects placed in the world, grabbed with the game's grab key or pushed.
// Watched like dropped items, but only near the player, and named by their shared reference id.
void DroppedItemService::UpdateWorldObjects(const std::chrono::steady_clock::time_point aNow, const std::vector<ItemSend>& acItemSends) noexcept
{
    // A dropped item's start or blast move, once it is its turn. A blast's sends were counted when the budget granted them.
    const auto sendItem = [&](const ItemSend& acSend, const bool aBlast)
    {
        const auto found = m_motion.find(acSend.Id);
        if (found == m_motion.end())
            return;
        Motion& motion = found->second;
        if (acSend.Start)
        {
            if (aBlast)
                spdlog::debug("DroppedItem: {} ({:X}) moved in a blast here; sending where it is", acSend.Id, acSend.RefFormId);
            else
                spdlog::debug("DroppedItem: {} ({:X}) is being moved here; sending where it is", acSend.Id, acSend.RefFormId);
            motion.LastLocalMove = aNow;
            motion.Moving = true;
            motion.Blast = aBlast;
        }
        motion.LastPosition = acSend.Position;
        motion.LastRotation = acSend.Rotation;
        motion.LastTurn = acSend.Turn;
        SendItemMove(acSend.Id, acSend.Position, acSend.Rotation, false, acSend.Start);
        if (!aBlast)
            m_moveBudget.Spend(aNow, 1);
    };

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    TESObjectCELL* pCell = pPlayer ? pPlayer->GetParentCellEx() : nullptr;
    if (!pCell)
    {
        for (const ItemSend& cSend : acItemSends)
            sendItem(cSend, false);
        return;
    }

    constexpr float cWatchRadius = 400.f;
    constexpr float cPropWatchRadius = 800.f;
    const glm::vec3 playerAt(pPlayer->position.x, pPlayer->position.y, pPlayer->position.z);
    m_bodyProbes = 0;

    // The other players' copies, where they stand, and their server ids: something they bump here is theirs to send, not ours.
    std::vector<glm::vec3> others;
    std::vector<uint32_t> otherIds;
    {
        auto view = m_world.view<FormIdComponent, PlayerComponent>();
        for (const auto cEntity : view)
        {
            if (const Actor* pOther = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(cEntity).Id)); pOther && pOther->formID != 0x14)
            {
                others.emplace_back(pOther->position.x, pOther->position.y, pOther->position.z);
                otherIds.push_back(view.get<PlayerComponent>(cEntity).Id);
            }
        }
    }

    if (aNow >= m_candidatesAt)
    {
        m_candidatesAt = aNow + std::chrono::seconds(1);
        m_objectCandidates.clear();

        static const Vector<FormType> s_types = []
        {
            Vector<FormType> types(kItemTypes);
            types.push_back(static_cast<FormType>(33));
            types.push_back(static_cast<FormType>(36));
            for (const int cType : {24, 28, 39, 40})
                types.push_back(static_cast<FormType>(cType));
            return types;
        }();

        // The player's cell and, outdoors, the attached cells around it that reach within the watched distance.
        Vector<TESObjectCELL*> cells;
        cells.push_back(pCell);
        const TES* pTES = TES::Get();
        if (TESWorldSpace* pWorldSpace = pPlayer->GetWorldSpace(); pWorldSpace && pTES)
        {
            for (const ChestSelection::Cell& cGrid : ChestSelection::CellsAround(pTES->currentGridX, pTES->currentGridY, 1))
            {
                if (!WorldObjectWatch::IsCellNear(cGrid.X, cGrid.Y, glm::vec2(playerAt.x, playerAt.y), cPropWatchRadius))
                    continue;
                TESObjectCELL* pAround = ModManager::Get()->GetCellFromCoordinates(cGrid.X, cGrid.Y, pWorldSpace, false);
                if (pAround && pAround != pCell && pAround->IsAttached())
                    cells.push_back(pAround);
            }
        }

        std::vector<WorldObjectWatch::Candidate> found;
        for (const TESObjectCELL* pObjectsCell : cells)
        {
            for (TESObjectREFR* pRef : pObjectsCell->GetRefsByFormTypes(s_types))
            {
                // Temporary references are dropped items, which have their own record above.
                if (!pRef || pRef->IsTemporary() || m_idByRef.count(pRef->formID))
                    continue;
                if (pRef->IsDisabled() || pRef->IsDeleted() || !pRef->GetNiNode())
                    continue;
                // An arrow or a spell can swing a hanging sign from further away than nearby clutter.
                const float cDistance = DistanceTo(pRef, playerAt);
                if (cDistance > cPropWatchRadius || (cDistance > cWatchRadius && !HasPropBodies(pRef)))
                    continue;
                if (!IsMovableObject(pRef) && !HasPropBodies(pRef))
                    continue;
                found.push_back({pRef->formID, WorldObjectWatch::Nearness(glm::vec3(pRef->position.x, pRef->position.y, pRef->position.z), playerAt, others)});
            }
        }
        if (found.size() > WorldObjectWatch::kMaxWatched)
        {
            static std::chrono::steady_clock::time_point s_nextFullLog{};
            if (aNow >= s_nextFullLog)
            {
                s_nextFullLog = aNow + std::chrono::seconds(30);
                spdlog::debug(
                    "WorldObject: {} movable objects within {:.0f} units; the {} nearest the players are watched", found.size(), cWatchRadius, WorldObjectWatch::kMaxWatched);
            }
        }
        m_objectCandidates = WorldObjectWatch::Pick(std::move(found));

        // Held for the other player until it is let go, and carried here until it comes to rest, wherever it is now.
        // A carried object dropped from the list would never send its rest, and the other side would let it fall mid-carry.
        for (auto& [formId, motion] : m_objectMotion)
        {
            motion.BodiesDriven.Release(motion.Bodies, aNow);
            const bool cSettling = aNow < motion.Bodies.HoldUntil;
            // Left to another game in a blast, it may still have to be sent from here.
            const bool cLeft = motion.LeftAt != std::chrono::steady_clock::time_point{} && aNow - motion.LeftAt < WorldObjectWatch::kClaimUntil;
            if (!motion.Held && !motion.Moving && !motion.BodiesDriven.On && !motion.Bodies.Moving && !cSettling && !cLeft)
                continue;
            if (std::find(m_objectCandidates.begin(), m_objectCandidates.end(), formId) != m_objectCandidates.end())
                continue;
            auto* pRef = Cast<TESObjectREFR>(TESForm::GetById(formId));
            if (motion.Held || motion.BodiesDriven.On || (pRef && pRef->GetNiNode()))
                m_objectCandidates.push_back(formId);
        }

        for (auto it = m_objectMotion.begin(); it != m_objectMotion.end();)
        {
            const bool cWatched = std::find(m_objectCandidates.begin(), m_objectCandidates.end(), it->first) != m_objectCandidates.end();
            it = cWatched ? std::next(it) : m_objectMotion.erase(it);
        }
    }

    std::array<UrGameObjectObservation, 32> observations{};
    const auto observed = GameExtension::TakeObjects(observations.data(), static_cast<uint32_t>(observations.size()));
    std::vector<uint32_t> observedMotion;
    for (size_t i = 0; i < observed; ++i)
    {
        const auto& state = observations[i];
        auto* ref = TESObjectREFR::GetByHandle(state.Handle);
        if (state.Loads != UI::LoadingScreensQueued() || !ref || ref->formID != state.Form || !ref->baseForm || ref->baseForm->formID != state.Base || ref->IsDisabled() ||
            ref->IsDeleted() || !ref->GetNiNode() || state.Cell != pCell->formID || ref->GetParentCellEx() != pCell || ref->extraData.Contains(ExtraDataType::AliasInstanceArray) ||
            m_idByRef.count(state.Form))
            continue;
        if (state.Adopt && ref->IsTemporary())
        {
            Inventory::Entry item{};
            if (!m_announced.count(state.Form) && !DroppedItemReplies::Reserved(m_pending, m_awaitingItems, state.Form) && ReadFloorItem(ref, item) && SendAdd(ref, item, false))
                m_announced.insert(state.Form);
            continue;
        }
        if (state.Adopt || ref->IsTemporary() || !IsMovableObject(ref))
            continue;
        const glm::vec3 previous{state.Previous[0], state.Previous[1], state.Previous[2]};
        if (!(glm::distance(previous, playerAt) <= 800.f) || !(DistanceTo(ref, playerAt) <= 800.f))
            continue;
        if (std::find(m_objectCandidates.begin(), m_objectCandidates.end(), state.Form) == m_objectCandidates.end())
            m_objectCandidates.push_back(state.Form);
        auto [entry, inserted] = m_objectMotion.try_emplace(state.Form);
        if (inserted)
        {
            entry->second.LastPosition = previous;
            entry->second.LastRotation = RotationOf(ref);
            entry->second.LastTurn = WorldTurnOf(ref->GetNiNode());
        }
        observedMotion.push_back(state.Form);
    }

    auto& modSystem = m_world.GetModSystem();
    // A move in between may be lost, as the next follows a frame later; where it starts and where it rests always arrive.
    const auto send = [&](TESObjectREFR* apRef, const uint32_t aFormId, const glm::vec3& acPosition, const glm::vec3& acRotation, const bool aAtRest, const bool aStart)
    {
        RequestWorldObjectMove request{};
        if (!modSystem.GetServerModId(aFormId, request.ObjectId))
            return;
        if (TESObjectCELL* pObjectCell = apRef->GetParentCellEx())
            modSystem.GetServerModId(pObjectCell->formID, request.CellId);
        if (TESWorldSpace* pWorldSpace = apRef->GetWorldSpace())
            modSystem.GetServerModId(pWorldSpace->formID, request.WorldSpaceId);
        request.Position = acPosition;
        request.Rotation = acRotation;
        request.AtRest = aAtRest;
        request.Tick = m_transport.GetClock().GetCurrentTick();
        if (const auto found = m_objectMotion.find(aFormId); found != m_objectMotion.end())
            AddThrow(request, found->second.OwnRelease, found->second.OwnFlight);
        m_transport.Send(request, aAtRest || aStart ? TiltedPhoques::kReliable : TiltedPhoques::kUnreliable);
    };
    const auto sendBodies =
        [&](TESObjectREFR* apRef, const uint32_t aFormId, const WorldObjectBodies::Poses& acPoses, const uint32_t aLayout, const bool aAtRest, const bool aStart)
    {
        RequestPropBodies request{};
        if (!modSystem.GetServerModId(aFormId, request.ObjectId))
            return;
        if (TESObjectCELL* pObjectCell = apRef->GetParentCellEx())
            modSystem.GetServerModId(pObjectCell->formID, request.CellId);
        if (TESWorldSpace* pWorldSpace = apRef->GetWorldSpace())
            modSystem.GetServerModId(pWorldSpace->formID, request.WorldSpaceId);
        request.Position = WorldPositionOf(apRef);
        request.Bodies.Layout = aLayout;
        request.Bodies.Bodies.assign(acPoses.begin(), acPoses.end());
        request.AtRest = aAtRest;
        request.Tick = m_transport.GetClock().GetCurrentTick();
        m_transport.Send(request, aAtRest || aStart ? TiltedPhoques::kReliable : TiltedPhoques::kUnreliable);
    };

    // Objects that start moving in this pass, judged together once the pass is done.
    struct Start
    {
        TESObjectREFR* pRef;
        uint32_t FormId;
        glm::vec3 Position;
        glm::vec3 Rotation;
        glm::mat3 Turn;
        float Distance;
        float Angle;
        //! The hinged bodies started, not the root (WorldObjectBodies.h), and how they lie.
        bool Bodies{};
        WorldObjectBodies::Poses BodyPoses{};
    };
    std::vector<Start> starts;
    //! Moves of objects sent as part of a blast, which wait for the blast's budget like its starts.
    std::vector<Start> blastMoves;

    for (const uint32_t cFormId : m_objectCandidates)
    {
        TESObjectREFR* pRef = Cast<TESObjectREFR>(TESForm::GetById(cFormId));
        const NiNode* pNode = pRef ? pRef->GetNiNode() : nullptr;
        if (!pRef || pRef->IsDeleted() || pRef->IsDisabled() || !pNode)
            continue;
        const bool cRootMoves = IsMovableObject(pRef);
        const bool cBodies = HasPropBodies(pRef);
        if (!cRootMoves && !cBodies)
            continue;

        const glm::vec3 position = WorldPositionOf(pRef);
        const glm::vec3 rotation = RotationOf(pRef);
        const glm::mat3 turn = WorldTurnOf(pNode);

        const auto found = m_objectMotion.find(cFormId);
        if (found == m_objectMotion.end())
        {
            // First seen: where it is now is where it was, and a second of quiet covers a cell still settling.
            Motion motion{};
            motion.LastPosition = position;
            motion.LastRotation = rotation;
            motion.LastTurn = turn;
            motion.QuietUntil = aNow + std::chrono::seconds(1);
            motion.FirstSeen = aNow;
            m_objectMotion.emplace(cFormId, motion);
            continue;
        }
        Motion& motion = found->second;

        if (cBodies)
        {
            // Another player's swing is let go once it rests there or its updates stop: physics settles the bodies from there.
            if (motion.BodiesDriven.Release(motion.Bodies, aNow))
            {
                spdlog::debug(
                    "WorldObject: {:X}'s hanging bodies {} over there; handed back to physics here", cFormId, motion.BodiesDriven.AtRest ? "came to rest" : "stopped being sent");
            }
            WorldObjectBodies::Poses poses;
            uint32_t layout{};
            // No fresh sample yet, after a long frame say: nothing is known this pass, so nothing rests.
            const auto cSample = PropHavok::Read(pRef, poses, layout);
            if (WorldObjectBodies::EndsSwing(cSample, layout, motion.BodiesLayout))
            {
                if (motion.Bodies.Moving && !motion.Bodies.Sent.empty())
                    sendBodies(pRef, cFormId, motion.Bodies.Sent, motion.BodiesLayout, true, false);
                motion.BodiesLayout = layout;
                motion.Bodies.Settle(poses);
            }
            else if (cSample == WorldObjectBodies::Sample::Read)
            {
                // Carried or pushed whole, its bodies swing with it: the root's moves show that.
                const bool cBusy = motion.BodiesDriven.On || motion.Held || motion.Moving || aNow < motion.QuietUntil;
                float distance = 0.f;
                float angle = 0.f;
                switch (motion.Bodies.Observe(poses, cBusy, aNow, distance, angle))
                {
                case WorldObjectBodies::Step::Start:
                {
                    Start start{pRef, cFormId, position, rotation, turn, distance, angle};
                    start.Bodies = true;
                    start.BodyPoses = std::move(poses);
                    starts.push_back(std::move(start));
                    break;
                }
                case WorldObjectBodies::Step::Move: sendBodies(pRef, cFormId, poses, layout, false, false); break;
                case WorldObjectBodies::Step::Rest:
                    sendBodies(pRef, cFormId, poses, layout, true, false);
                    spdlog::debug("WorldObject: {:X}'s hanging bodies came to rest", cFormId);
                    break;
                default: break;
                }
            }
        }
        if (!cRootMoves)
            continue;
        const bool cHeldHere = PollHeld(pRef, motion, position);
        if (motion.Caught.Poll(motion.Held && motion.RemoteThrow.IsFlying(), cHeldHere))
            CatchHere(pRef, motion, aNow, "WorldObject", cFormId);

        // A put-down is let go half a second after it arrives, so the player here can take the object at once.
        // A move that just stops arriving is let go after two seconds.
        const auto cHoldFor = motion.RemoteAtRest ? std::chrono::milliseconds(500) : std::chrono::milliseconds(2000);
        if (motion.Held && aNow - motion.LastRemote > cHoldFor)
        {
            PutAtNewest(pRef, motion, aNow);
            pRef->SetMotionType(TESObjectREFR::kMotionDynamic);
            motion.Held = false;
            motion.QuietUntil = aNow + cHoldFor;
            motion.StopShowing();
            motion.RemoteThrow = {};
            // Where its 3D is drawn, against the last place the other side put it: what this player sees of the carry.
            spdlog::info("WorldObject: {:X} {}; handed back to physics here ({} of its {} moves here rebuilt its 3D; its 3D is {:.0f} units from the last place it was put)",
                         cFormId, motion.RemoteAtRest ? "was put down over there" : "stopped being moved over there", motion.HeldRebuilds, motion.HeldMoves,
                         glm::distance(position, motion.RemoteTarget));
            motion.HeldMoves = 0;
            motion.HeldRebuilds = 0;
        }

        if (motion.Held || aNow < motion.QuietUntil)
        {
            motion.LastPosition = position;
            motion.LastRotation = rotation;
            motion.LastTurn = turn;
            motion.OwnRelease.Rest();
            continue;
        }
        // At rest, it moves once it leaves the place it rests at.
        const bool cMovedHere = glm::distance(position, motion.LastPosition) > kMovedDistance || AngleBetween(motion.LastTurn, turn) > kMovedAngle;
        NoteFlight(pRef, motion, position, turn, Milliseconds(aNow), cHeldHere, cMovedHere || motion.Moving);

        if (!motion.Moving)
        {
            const float cDistance = glm::distance(position, motion.LastPosition);
            const float cAngle = AngleBetween(motion.LastTurn, turn);
            switch (WorldObjectWatch::JudgeResting(cDistance > kRestartDistance || cAngle > kRestartAngle, motion.LeftAt, motion.LastRemote, aNow))
            {
            case WorldObjectWatch::Resting::Starts: starts.push_back({pRef, cFormId, position, rotation, turn, cDistance, cAngle}); break;
            case WorldObjectWatch::Resting::SendsPlace:
                // Both games may have left it in the blast, and it landed before either claimed it.
                motion.LeftAt = {};
                motion.LastPosition = position;
                motion.LastRotation = rotation;
                motion.LastTurn = turn;
                motion.OwnRelease.Rest();
                send(pRef, cFormId, position, rotation, true, false);
                spdlog::debug("WorldObject: {:X} was left to another player's game in a blast and came to rest here with no move of it from there; sending where it lies", cFormId);
                break;
            default: break;
            }
            continue;
        }

        if (cMovedHere)
        {
            motion.LastLocalMove = aNow;
            // Waits for its turn with the blast's other sends; a move is measured from the last place sent until then.
            if (motion.Blast)
            {
                blastMoves.push_back({pRef, cFormId, position, rotation, turn});
                continue;
            }
            motion.LastPosition = position;
            motion.LastRotation = rotation;
            motion.LastTurn = turn;
            send(pRef, cFormId, position, rotation, false, false);
            m_moveBudget.Spend(aNow, 1);
        }
        // A second of stillness: a hand pausing mid-carry is not a put-down, which the other side lets go of at once.
        else if (aNow - motion.LastLocalMove >= std::chrono::milliseconds(1000))
        {
            // A restart is measured from where it rests.
            motion.Moving = false;
            motion.Blast = false;
            motion.LastPosition = position;
            motion.LastRotation = rotation;
            motion.LastTurn = turn;
            motion.OwnRelease.Rest();
            send(pRef, cFormId, position, rotation, true, false);
            spdlog::debug("WorldObject: {:X} came to rest at ({:.0f})", cFormId, position);
        }
    }

    // An object whose root and bodies both started is one start, its root's: the bodies swing with it.
    for (auto it = starts.begin(); it != starts.end();)
    {
        const bool cRootToo = it->Bodies && std::any_of(starts.begin(), starts.end(), [&](const Start& acOther) { return !acOther.Bodies && acOther.FormId == it->FormId; });
        if (cRootToo)
        {
            m_objectMotion[it->FormId].Bodies.Settle(it->BodyPoses);
            it = starts.erase(it);
        }
        else
            ++it;
    }

    // Many objects starting at once is a blast or a cell settling, not a hand; dropped items knocked about count with them.
    // Those only just watched are their cell settling as it attaches: nobody sends them.
    const auto cItemStarts = static_cast<size_t>(std::count_if(acItemSends.begin(), acItemSends.end(), [](const ItemSend& acSend) { return acSend.Start; }));
    if (WorldObjectWatch::IsBlast(starts.size(), cItemStarts))
    {
        for (const ItemSend& cSend : acItemSends)
        {
            if (const auto found = m_motion.find(cSend.Id); cSend.Start && found != m_motion.end())
                found->second.BlastUntil = aNow + WorldObjectWatch::kBlastFor;
        }
        for (auto it = starts.begin(); it != starts.end();)
        {
            Motion& motion = m_objectMotion[it->FormId];
            if (!WorldObjectWatch::IsSettling(motion.FirstSeen, aNow))
            {
                motion.BlastUntil = aNow + WorldObjectWatch::kBlastFor;
                ++it;
                continue;
            }
            motion.LastPosition = it->Position;
            motion.LastRotation = it->Rotation;
            motion.LastTurn = it->Turn;
            motion.QuietUntil = aNow + std::chrono::seconds(2);
            if (it->Bodies)
                motion.Bodies.Settle(it->BodyPoses);
            it = starts.erase(it);
        }
    }

    const auto startSending = [&](const Start& acStart, const bool aBlast)
    {
        Motion& motion = m_objectMotion[acStart.FormId];
        if (acStart.Bodies)
        {
            spdlog::debug(
                "WorldObject: {:X}'s hanging bodies swing here; sending how they lie ({:.0f} units, {:.2f} rad from where they rested)", acStart.FormId, acStart.Distance,
                acStart.Angle);
            motion.Bodies.Begin(acStart.BodyPoses, aNow);
            sendBodies(acStart.pRef, acStart.FormId, acStart.BodyPoses, motion.BodiesLayout, false, true);
            return;
        }

        if (aBlast)
            spdlog::debug("WorldObject: {:X} moved in a blast here; sending where it is (moved {:.0f} units)", acStart.FormId, acStart.Distance);
        else
            spdlog::debug("WorldObject: {:X} is being moved here; sending where it is (moved {:.0f} units, turned {:.2f} rad)", acStart.FormId, acStart.Distance, acStart.Angle);
        motion.LastPosition = acStart.Position;
        motion.LastRotation = acStart.Rotation;
        motion.LastTurn = acStart.Turn;
        motion.LastLocalMove = aNow;
        motion.Moving = true;
        motion.Blast = aBlast;
        motion.LeftAt = {};
        send(acStart.pRef, acStart.FormId, acStart.Position, acStart.Rotation, false, true);
    };

    // A blast's starts and moves, sent nearest first as the budget allows; the others wait for a later pass.
    std::vector<const Start*> blastSends;
    std::vector<float> blastDistances;

    const uint32_t cPlayerId = m_transport.GetLocalPlayerId();
    for (const Start& start : starts)
    {
        Motion& motion = m_objectMotion[start.FormId];
        const bool cBlast = aNow < motion.BlastUntil;

        // Nearer another player's copy than this player: their body bumped it here, or in a blast their game sends it.
        // Their own game does not push clutter with a VR body, so it is not sent.
        // Unless this player moved it a moment ago: then it is a carry going on next to them, a hand-over say.
        const float cMine = glm::distance(start.Position, playerAt);
        const bool cCarriedHere = aNow - (start.Bodies ? motion.Bodies.LastMove : motion.LastLocalMove) < std::chrono::seconds(3);
        std::vector<WorldObjectWatch::Other> theirs;
        theirs.reserve(others.size());
        for (size_t i = 0; i < others.size(); ++i)
            theirs.push_back({glm::distance(start.Position, others[i]), otherIds[i]});
        const bool cTheirs = !cCarriedHere && WorldObjectWatch::IsTheirs(cMine, cPlayerId, theirs, cBlast);
        // Left to another game in a blast, yet still moving here with no move of it from there: nobody else sends it.
        const bool cClaimed = cTheirs && WorldObjectWatch::ClaimsLeftObject(motion.LeftAt, motion.LastRemote, aNow);
        // An articulated swing can start beside another player without its root moving.
        if (cTheirs && !cClaimed && !start.Bodies && std::find(observedMotion.begin(), observedMotion.end(), start.FormId) == observedMotion.end())
        {
            motion.LastPosition = start.Position;
            motion.LastRotation = start.Rotation;
            motion.LastTurn = start.Turn;
            motion.QuietUntil = aNow + std::chrono::seconds(1);
            if (cBlast && (motion.LeftAt == std::chrono::steady_clock::time_point{} || aNow - motion.LeftAt >= WorldObjectWatch::kClaimUntil))
                motion.LeftAt = aNow;
            spdlog::debug("WorldObject: {:X} moved next to another player's copy; not sent", start.FormId);
            continue;
        }
        if (cClaimed)
            spdlog::debug("WorldObject: {:X} was left to another player's game in a blast, but no move of it came; sending it from here", start.FormId);

        if (cBlast)
        {
            // Still part of the blast while it waits.
            motion.BlastUntil = aNow + WorldObjectWatch::kBlastFor;
            blastSends.push_back(&start);
            blastDistances.push_back(cMine);
            continue;
        }
        startSending(start, false);
        m_moveBudget.Spend(aNow, 1);
    }

    const size_t cBlastStarts = blastSends.size();
    for (const Start& move : blastMoves)
    {
        blastSends.push_back(&move);
        blastDistances.push_back(glm::distance(move.Position, playerAt));
    }

    // Dropped items sent in a blast, or starting while one goes on, wait for its budget with the objects.
    const size_t cObjectSends = blastSends.size();
    std::vector<const ItemSend*> itemBlastSends;
    for (const ItemSend& cSend : acItemSends)
    {
        const auto found = m_motion.find(cSend.Id);
        if (found == m_motion.end())
            continue;
        if (cSend.Start && aNow >= found->second.BlastUntil)
        {
            sendItem(cSend, false);
            continue;
        }
        if (cSend.Start)
            found->second.BlastUntil = aNow + WorldObjectWatch::kBlastFor;
        itemBlastSends.push_back(&cSend);
        blastDistances.push_back(glm::distance(cSend.Position, playerAt));
    }

    if (blastDistances.empty())
        return;
    for (const size_t cIndex : WorldObjectWatch::Nearest(blastDistances, m_moveBudget.Grant(aNow, blastDistances.size())))
    {
        if (cIndex >= cObjectSends)
        {
            sendItem(*itemBlastSends[cIndex - cObjectSends], true);
            continue;
        }
        const Start& waiting = *blastSends[cIndex];
        if (cIndex < cBlastStarts)
        {
            startSending(waiting, true);
            continue;
        }
        Motion& motion = m_objectMotion[waiting.FormId];
        motion.LastPosition = waiting.Position;
        motion.LastRotation = waiting.Rotation;
        motion.LastTurn = waiting.Turn;
        send(waiting.pRef, waiting.FormId, waiting.Position, waiting.Rotation, false, false);
    }
}

void DroppedItemService::OnNotifyWorldObjectMove(const NotifyWorldObjectMove& acMessage) noexcept
{
    if (!acMessage.Taken &&
        !ClientTrust::IsUsableTransform(acMessage.Position.x, acMessage.Position.y, acMessage.Position.z, acMessage.Rotation.x, acMessage.Rotation.y, acMessage.Rotation.z))
        return;
    const uint32_t cFormId = m_world.GetModSystem().GetGameId(acMessage.ObjectId);
    TESObjectREFR* pRef = cFormId ? Cast<TESObjectREFR>(TESForm::GetById(cFormId)) : nullptr;

    // Taken into another player's inventory (see OnItemPickedUp): gone from this world too.
    // Disabled, not deleted: it is a plugin's placed reference, and disabled is how this save keeps it.
    // Only an item or other movable object (never an actor, a door or a building): a message naming anything else is not obeyed.
    if (acMessage.Taken)
    {
        if (pRef && !IsMovableObject(pRef))
        {
            spdlog::warn("WorldObject: {:X} reported taken by another player, but it is no item or movable object; left as it is", cFormId);
            return;
        }
        if (pRef && !pRef->IsDeleted() && !pRef->IsDisabled())
        {
            if (const auto held = m_objectMotion.find(cFormId); held != m_objectMotion.end() && held->second.Held)
                pRef->SetMotionType(TESObjectREFR::kMotionDynamic);
            m_objectMotion.erase(cFormId);
            pRef->Disable();
            spdlog::info("WorldObject: {:X} was taken by another player; removed here", cFormId);
        }
        return;
    }

    // Not loaded here: the server sends where it was left again when this player comes near.
    if (!pRef || pRef->IsDeleted() || pRef->IsDisabled() || !pRef->GetNiNode() || !IsMovableObject(pRef))
        return;

    const auto now = std::chrono::steady_clock::now();
    Motion& motion = m_objectMotion[cFormId];
    if (!motion.RemoteOrder.Take(acMessage.Tick))
        return; // sent before an update already taken
    // Caught here and still in the hand: this game moves it.
    if (motion.Caught.IsCaught())
        return;
    if (!motion.Held)
    {
        pRef->SetMotionType(TESObjectREFR::kMotionKeyframed);
        motion.Held = true;
        spdlog::info("WorldObject: {:X} is being moved over there; holding it here and following", cFormId);
    }

    const glm::vec3 position(acMessage.Position);
    // While it flies here by this game's physics, its thrown moves are not drawn.
    const bool cFlies =
        NoteThrow(pRef, motion, now, acMessage.Tick, acMessage.Thrown, {acMessage.Velocity, acMessage.Spin}, acMessage.AtRest, position) == HeldObject::Launch::Step::Skip;
    if (!cFlies)
        ++motion.HeldMoves;
    if (!cFlies && !Follow(pRef, motion, now, position, acMessage.Rotation, acMessage.AtRest, acMessage.Tick))
    {
        const NiNode* pNodeBefore = pRef->GetNiNode();
        PlaceMovedObject(pRef, position, acMessage.Rotation);
        if (const NiNode* pNodeAfter = pRef->GetNiNode(); pNodeBefore && pNodeAfter != pNodeBefore)
        {
            ++motion.HeldRebuilds;
            if (pNodeAfter)
                pRef->SetMotionType(TESObjectREFR::kMotionKeyframed);
        }
        // Thrown in the first move of it seen here: it flies from here at once.
        if (motion.RemoteThrow.Due(std::numeric_limits<double>::infinity()))
            LaunchHere(pRef, motion, "WorldObject", cFormId);
    }

    motion.LastRemote = now;
    motion.LastPosition = position;
    motion.RemoteTarget = position;
    motion.LastRotation = acMessage.Rotation;
    motion.Moving = false;
    motion.Blast = false;
    motion.LeftAt = {};
    motion.RemoteAtRest = acMessage.AtRest;
    motion.MoverPlayerId = acMessage.MoverPlayerId;

    if (std::find(m_objectCandidates.begin(), m_objectCandidates.end(), cFormId) == m_objectCandidates.end())
        m_objectCandidates.push_back(cFormId);
}

bool DroppedItemService::HasPropBodies(TESObjectREFR* apRef) noexcept
{
    if (!apRef || !apRef->baseForm || !WorldObjectBodies::IsBodiesType(static_cast<uint8_t>(apRef->baseForm->formType)))
        return false;
    const uint32_t cBaseId = apRef->baseForm->formID;
    if (const auto known = m_hasBodiesByBase.find(cBaseId); known != m_hasBodiesByBase.end())
        return known->second;
    // Asked again later: soon while its collision is not in the world yet, after a minute when it hung nothing that could swing
    // (a body keyframed while carried, a trap not yet sprung).
    const auto cNow = std::chrono::steady_clock::now();
    if (const auto again = m_bodiesAskAgainAt.find(cBaseId); again != m_bodiesAskAgainAt.end() && cNow < again->second)
        return false;
    if (m_bodyProbes >= 2)
        return false;
    ++m_bodyProbes;
    const auto cCount = PropHavok::CountBodies(apRef);
    if (!cCount)
        return false;
    if (*cCount <= 0)
    {
        m_bodiesAskAgainAt[cBaseId] = cNow + (*cCount < 0 ? std::chrono::seconds(5) : std::chrono::seconds(60));
        return false;
    }
    m_bodiesAskAgainAt.erase(cBaseId);
    m_hasBodiesByBase.emplace(cBaseId, true);
    spdlog::debug("WorldObject: {:X} ({:X}) hangs {} bodies that can swing; their swing is passed on", apRef->formID, cBaseId, *cCount);
    return true;
}

void DroppedItemService::OnNotifyPropBodies(const NotifyPropBodies& acMessage) noexcept
{
    const uint32_t cFormId = m_world.GetModSystem().GetGameId(acMessage.ObjectId);
    TESObjectREFR* pRef = cFormId ? Cast<TESObjectREFR>(TESForm::GetById(cFormId)) : nullptr;
    // Not loaded here: hanging bodies settle by themselves, so nothing is kept for later.
    if (!acMessage.Bodies.Layout || !pRef || pRef->IsDeleted() || pRef->IsDisabled() || !pRef->GetNiNode() || !HasPropBodies(pRef))
        return;

    const auto now = std::chrono::steady_clock::now();
    auto found = m_objectMotion.find(cFormId);
    if (found == m_objectMotion.end())
    {
        Motion motion{};
        motion.LastPosition = WorldPositionOf(pRef);
        motion.LastRotation = RotationOf(pRef);
        motion.LastTurn = WorldTurnOf(pRef->GetNiNode());
        motion.QuietUntil = now + std::chrono::seconds(1);
        motion.FirstSeen = now;
        found = m_objectMotion.emplace(cFormId, motion).first;
    }
    Motion& motion = found->second;
    const bool cWasDriven = motion.BodiesDriven.On;
    if (cWasDriven && motion.BodiesDriven.Layout != acMessage.Bodies.Layout)
    {
        if (acMessage.Tick && motion.BodiesDriven.Tick && acMessage.Tick <= motion.BodiesDriven.Tick)
            return;
        motion.BodiesDriven = {};
    }
    if (!motion.BodiesDriven.Take(WorldObjectBodies::Poses(acMessage.Bodies.Bodies.begin(), acMessage.Bodies.Bodies.end()), acMessage.AtRest, acMessage.Tick, now))
        return; // older than the update shown
    motion.BodiesDriven.Layout = acMessage.Bodies.Layout;
    // Both players swung it at once: the server passes on one of them, so this game stops sending its own.
    if (motion.Bodies.Moving)
        spdlog::debug("WorldObject: {:X}'s hanging bodies swing here and over there; following theirs", cFormId);
    motion.Bodies.GiveWay();
    if (!cWasDriven)
        spdlog::debug("WorldObject: received a remote swing for {:X}", cFormId);

    if (std::find(m_objectCandidates.begin(), m_objectCandidates.end(), cFormId) == m_objectCandidates.end())
        m_objectCandidates.push_back(cFormId);
}

void DroppedItemService::DrivePropBodies() noexcept
{
    const auto cNow = std::chrono::steady_clock::now();
    std::vector<std::pair<uint32_t, WorldObjectBodies::Driven>> queued;
    for (const auto& [formId, motion] : m_objectMotion)
        if (motion.BodiesDriven.On && !motion.BodiesDriven.LetGo(cNow))
            queued.emplace_back(formId, motion.BodiesDriven);
    PropHavok::Queue(std::move(queued));
}

void DroppedItemService::OnNotifyDroppedItemMove(const NotifyDroppedItemMove& acMessage) noexcept
{
    if (!ClientTrust::IsUsableTransform(acMessage.Position.x, acMessage.Position.y, acMessage.Position.z, acMessage.Rotation.x, acMessage.Rotation.y, acMessage.Rotation.z))
        return;
    const auto it = m_refById.find(acMessage.Id);
    if (it == m_refById.end())
        return; // not placed here yet; the server hands over its resting place with the item

    TESObjectREFR* pRef = RefOf(it->second);
    Motion& motion = m_motion[acMessage.Id];
    if (!pRef || !IsStillOurs(pRef, motion) || pRef->IsDeleted())
        return;
    if (!motion.RemoteOrder.Take(acMessage.Tick))
        return; // sent before an update already taken
    // Caught here and still in the hand: this game moves it.
    if (motion.Caught.IsCaught())
        return;

    const auto now = std::chrono::steady_clock::now();

    // Held still while the other side moves it, or local physics pulls it down between updates and it stutters.
    // Also for a lone "put down": it is placed exactly, then let go.
    if (!motion.Held)
    {
        pRef->SetMotionType(TESObjectREFR::kMotionKeyframed);
        motion.Held = true;
        spdlog::info("DroppedItem: {} ({:X}) is being moved over there; holding it here and following", acMessage.Id, it->second);
    }

    const glm::vec3 position(acMessage.Position);
    // While it flies here by this game's physics, its thrown moves are not drawn.
    const bool cFlies = NoteThrow(pRef, motion, now, acMessage.Tick, acMessage.Thrown && !acMessage.Fixed, {acMessage.Velocity, acMessage.Spin}, acMessage.AtRest, position) ==
                        HeldObject::Launch::Step::Skip;
    if (!cFlies && !Follow(pRef, motion, now, position, acMessage.Rotation, acMessage.AtRest, acMessage.Tick))
    {
        const NiNode* pNodeBefore = pRef->GetNiNode();
        PlaceMovedObject(pRef, position, acMessage.Rotation);

        // A rebuilt 3D has a dynamic body again, which would fall, so it is held again.
        // Logged once, to learn whether a move ever rebuilds it.
        if (const NiNode* pNodeAfter = pRef->GetNiNode(); pNodeBefore && pNodeAfter && pNodeAfter != pNodeBefore)
        {
            pRef->SetMotionType(TESObjectREFR::kMotionKeyframed);
            static bool s_reported = false;
            if (!s_reported)
            {
                s_reported = true;
                spdlog::info("DroppedItem: moving {} rebuilt its 3D; holding it again after each move", acMessage.Id);
            }
        }
        // Thrown in the first move of it seen here: it flies from here at once.
        if (motion.RemoteThrow.Due(std::numeric_limits<double>::infinity()))
            LaunchHere(pRef, motion, "DroppedItem", acMessage.Id);
    }

    motion.LastRemote = now;
    motion.LastPosition = position;
    motion.LastRotation = acMessage.Rotation;
    motion.Moving = false;
    motion.Blast = false;
    // Let go two seconds after the last update (OnUpdate), not now: a still hand also sends "at rest".
    motion.RemoteAtRest = acMessage.AtRest;
    motion.MoverPlayerId = acMessage.MoverPlayerId;
    motion.RemoteFixed = SharedHomes::KeptStill(acMessage.AtRest, acMessage.Fixed);
    motion.WasRemoteFixed = motion.WasRemoteFixed || motion.RemoteFixed;
}

void DroppedItemService::HoldFixed(TESObjectREFR* apRef, Motion& aMotion) noexcept
{
    if (!apRef->GetNiNode())
        return;
    PlaceMovedObject(apRef, aMotion.LastPosition, aMotion.LastRotation);
    // After the move, which can rebuild the 3D with a dynamic body.
    apRef->SetMotionType(TESObjectREFR::kMotionKeyframed);
    aMotion.FixPending = false;
    // Its own settling there is not a move of this game's.
    aMotion.QuietUntil = std::max(aMotion.QuietUntil, std::chrono::steady_clock::now() + std::chrono::seconds(2));
    spdlog::debug("DroppedItem: {:X} put where it was left and held still there, as its owner's script holds it", apRef->formID);
}

// Shown on the mover's clock rather than put where it is now. False when this update is to be put in place now: the
// first update of a carry, a jump, or no 3D.
bool DroppedItemService::Follow(
    TESObjectREFR* apRef, Motion& aMotion, const std::chrono::steady_clock::time_point aNow, const glm::vec3& acPosition, const glm::vec3& acRotation, const bool aAtRest,
    const uint64_t aTick) noexcept
{
    const glm::vec3 cNow(apRef->position.x, apRef->position.y, apRef->position.z);
    if (!apRef->GetNiNode() || glm::distance(cNow, acPosition) >= kMoveInPlaceDistance)
    {
        aMotion.StopShowing();
        return false;
    }
    // The first update with a tick: what was put on arrival times before it cannot share a line with ticks.
    const bool cWasTimed = aMotion.Line.IsTimed();
    const double cArrival = Milliseconds(aNow);
    const double cTime = aMotion.Line.Note(cArrival, aTick);
    if (aMotion.Line.IsTimed() != cWasTimed)
        aMotion.Shown.Clear();
    const bool cFirst = aMotion.Shown.IsEmpty();
    aMotion.Shown.Add(cTime, {acPosition, HeldObject::FromAngles(acRotation)}, aAtRest);
    return !cFirst;
}

// Every frame, an object another player moves is put where their game had it one playback delay ago, as far behind as
// their copy is shown, so the object and the body carrying it move together. An object that sits in a hand of the copy
// is drawn from that hand.
void DroppedItemService::ShowHeldObjects(const std::chrono::steady_clock::time_point aNow) noexcept
{
    bool anyShown = false;
    for (const auto* pMotions : {&m_motion, &m_objectMotion})
        for (const auto& [id, motion] : *pMotions)
            anyShown = anyShown || (motion.Held && !motion.Shown.IsEmpty());
    if (!anyShown)
        return;

    const double cNow = Milliseconds(aNow);

    // The other players' copies: how far behind they are shown, and their hands, put a frame ahead of where the last pass left them.
    struct Mover
    {
        uint32_t FormId{};
        uint32_t PlayerId{};
        glm::vec3 Position{};
        double Delay{};
        std::array<HeldObject::Pose, 2> Hands{};
        std::array<bool, 2> HasHand{};
    };
    std::vector<Mover> movers;
    {
        auto view = m_world.view<FormIdComponent, PlayerComponent, InterpolationComponent>();
        for (const auto cEntity : view)
        {
            Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(cEntity).Id));
            if (!pActor || pActor->formID == 0x14)
                continue;
            Mover mover{};
            mover.FormId = pActor->formID;
            mover.PlayerId = view.get<PlayerComponent>(cEntity).Id;
            mover.Position = {pActor->position.x, pActor->position.y, pActor->position.z};
            const PlaybackClock& cClock = view.get<InterpolationComponent>(cEntity).Clock;
            mover.Delay = cClock.IsRunning() ? cClock.GetTargetDelay() : 0.0;
            // Only a hand posed from the player's own pose: an animation hand is not where they hold anything.
            static const std::array<BSFixedString, 2> s_hands{BSFixedString("NPC L Hand [LHnd]"), BSFixedString("NPC R Hand [RHnd]")};
            NiNode* pRoot = pActor->GetNiNode();
            for (size_t side = 0; side < s_hands.size(); ++side)
            {
                if (!PosedHands::IsPosed(pActor->formID, side))
                    continue;
                if (const NiNode* pHand = FindNamed(pRoot, s_hands[side]))
                {
                    // A transform read mid-write is no hand.
                    const HeldObject::Pose cRead = WorldPoseOf(pHand);
                    mover.HasHand[side] = HeldObject::IsFinite(cRead);
                    if (mover.HasHand[side])
                        mover.Hands[side] = m_handLeads[(static_cast<uint64_t>(pActor->formID) << 1) | side].Lead(cNow, cRead);
                }
            }
            movers.push_back(mover);
        }
    }

    // A hand nobody has read for a second is forgotten.
    for (auto lead = m_handLeads.begin(); lead != m_handLeads.end();)
        lead = cNow - lead->second.GetLastRead() > 1000.0 ? m_handLeads.erase(lead) : std::next(lead);

    const auto show = [&](TESObjectREFR* apRef, Motion& aMotion, const char* acpKind, const uint32_t acId)
    {
        // As far behind as the copy of the player moving it is shown, so a carry keeps pace with the body carrying it.
        // Not named, or that copy not here: the nearest copy.
        const auto cNamed = HeldMover::Named(movers, aMotion.MoverPlayerId);
        const Mover* pNamed = cNamed ? &movers[*cNamed] : nullptr;
        const glm::vec3 cNewest = aMotion.Shown.Newest().Position;
        const Mover* pMover = pNamed;
        for (const Mover& mover : movers)
            if (!pNamed && (!pMover || glm::distance(mover.Position, cNewest) < glm::distance(pMover->Position, cNewest)))
                pMover = &mover;
        const double cMoverDelay = pMover ? std::min(pMover->Delay, HeldObject::Playback::kMaxDelay) : 0.0;
        // On the other player's ticks: the moment their clock gives, behind by the same as their copy. On arrival times: as far behind as the gaps allow.
        double cMoment = cNow;
        double cDelay = 0.0;
        if (aMotion.Line.IsTimed())
            cMoment = aMotion.Line.Moment(cNow, cMoverDelay);
        else
            cDelay = aMotion.ShownDelay.Follow(cNow, std::max(aMotion.Shown.GetDelay(), cMoverDelay));
        const HeldObject::Pose cPath = aMotion.Shown.Show(cMoment, cDelay);
        aMotion.ShownAt = cMoment - cDelay;
        aMotion.HasShownAt = true;
        // Thrown over there and flying here by this game's physics: not drawn.
        if (aMotion.RemoteThrow.IsFlying())
            return;

        HeldObject::Pose drawn = cPath;
        // Put down: it ends exactly where it was put, not in the hand.
        const HeldObject::Pose* pHand = nullptr;
        uint64_t handKey = 0;
        if (!aMotion.Shown.IsRestShown(cMoment, cDelay))
        {
            // The hand holding it keeps it while that hand is there, even with another one nearer (two hands, a pass).
            // Only the named mover's hands count; with none named, every copy's.
            const uint64_t cHeldBy = aMotion.InHand.GetHandKey();
            for (const Mover& mover : movers)
            {
                const auto cSide = static_cast<size_t>(cHeldBy & 1);
                if (cHeldBy && (cHeldBy >> 1) == mover.FormId && mover.HasHand[cSide] && (!pNamed || &mover == pNamed))
                {
                    pHand = &mover.Hands[cSide];
                    handKey = cHeldBy;
                }
            }
            const bool cHeldHandHere = handKey != 0;
            float nearest = HeldObject::Grip::kReach;
            for (const Mover& mover : movers)
            {
                if (pNamed && &mover != pNamed)
                    continue;
                for (size_t side = 0; side < mover.Hands.size() && !cHeldHandHere; ++side)
                {
                    const float cDistance = glm::distance(mover.Hands[side].Position, cPath.Position);
                    if (mover.HasHand[side] && cDistance <= nearest)
                    {
                        nearest = cDistance;
                        pHand = &mover.Hands[side];
                        handKey = (static_cast<uint64_t>(mover.FormId) << 1) | side;
                    }
                }
            }
        }
        const bool cWasHeld = aMotion.InHand.IsHeld();
        drawn = aMotion.InHand.Update(cNow, handKey, pHand, cPath);
        if (cWasHeld && !aMotion.InHand.IsHeld())
            spdlog::debug("{}: {:X} left the hand of the other player's copy", acpKind, acId);
        else if (!cWasHeld && aMotion.InHand.IsHeld())
            spdlog::debug(
                "{}: {:X} is in a hand of {:X}'s copy, {:.0f} units from it; drawn from that hand", acpKind, acId, static_cast<uint32_t>(aMotion.InHand.GetHandKey() >> 1),
                aMotion.InHand.GetGripDistance());

        // Never hand the game a broken place; where it already is (a still hand): nothing to move.
        if (!HeldObject::IsFinite(drawn))
            return;
        // The let-go is shown now: it flies on from where it is drawn.
        const bool cLaunch = aMotion.RemoteThrow.Due(cMoment - cDelay);
        if (!aMotion.HasDrawn || glm::distance(drawn.Position, aMotion.Drawn.Position) >= 0.01f ||
            1.0 - std::fabs(static_cast<double>(glm::dot(drawn.Rotation, aMotion.Drawn.Rotation))) >= 1e-6)
        {
            const NiNode* pNodeBefore = apRef->GetNiNode();
            PlaceMovedObject(apRef, drawn.Position, HeldObject::ToAngles(drawn.Rotation));
            if (const NiNode* pNodeAfter = apRef->GetNiNode(); pNodeBefore && pNodeAfter != pNodeBefore)
            {
                ++aMotion.HeldRebuilds;
                if (pNodeAfter)
                    apRef->SetMotionType(TESObjectREFR::kMotionKeyframed);
            }
            aMotion.Drawn = drawn;
            aMotion.HasDrawn = true;
        }
        if (cLaunch)
            LaunchHere(apRef, aMotion, acpKind, acId);
    };

    for (auto& [id, motion] : m_motion)
    {
        if (!motion.Held || motion.Shown.IsEmpty())
            continue;
        const auto it = m_refById.find(id);
        TESObjectREFR* pRef = it != m_refById.end() ? RefOf(it->second) : nullptr;
        if (pRef && IsStillOurs(pRef, motion) && !pRef->IsDeleted() && !pRef->IsDisabled() && pRef->GetNiNode())
            show(pRef, motion, "DroppedItem", id);
    }
    for (auto& [formId, motion] : m_objectMotion)
    {
        if (!motion.Held || motion.Shown.IsEmpty())
            continue;
        TESObjectREFR* pRef = Cast<TESObjectREFR>(TESForm::GetById(formId));
        if (pRef && !pRef->IsDeleted() && !pRef->IsDisabled() && pRef->GetNiNode() && IsMovableObject(pRef))
            show(pRef, motion, "WorldObject", formId);
    }
}

// Let go of while shown behind the mover or in a hand: put where the mover last had it first, so physics takes over
// from their place and not from up to a grip's distance away.
void DroppedItemService::PutAtNewest(TESObjectREFR* apRef, Motion& aMotion, const std::chrono::steady_clock::time_point aNow) noexcept
{
    // Flying here by this game's physics: left where it has it.
    if (!aMotion.HasDrawn || aMotion.Shown.IsEmpty() || aMotion.RemoteThrow.IsFlying() || !apRef->GetNiNode())
        return;
    // Where the other player's game left it, whichever moment is shown now: the landing spot is theirs.
    const HeldObject::Pose cNewest = aMotion.Shown.Newest();
    PlaceMovedObject(apRef, cNewest.Position, HeldObject::ToAngles(cNewest.Rotation));
}

HeldObject::Launch::Step DroppedItemService::NoteThrow(
    TESObjectREFR* apRef, Motion& aMotion, const std::chrono::steady_clock::time_point aNow, const uint64_t aTick, const bool aThrown, const HeldObject::Flight& acFlight,
    const bool aAtRest, const glm::vec3& acPosition) noexcept
{
    // On the line Follow puts it on: a thrown move always carries the mover's tick.
    const double cTime = aTick ? static_cast<double>(aTick) : Milliseconds(aNow);
    const auto cStep = aMotion.RemoteThrow.Note(cTime, aThrown, acFlight, aAtRest, acPosition);
    if (cStep != HeldObject::Launch::Step::TakeBack)
        return cStep;
    // Caught or put down over there, or flown here too long: held again, and drawn from where its flight here left it onto the
    // mover's updates.
    PropHavok::CancelLaunch(apRef->formID);
    apRef->SetMotionType(TESObjectREFR::kMotionKeyframed);
    aMotion.Shown.Clear();
    aMotion.InHand = {};
    aMotion.HasDrawn = false;
    if (const NiNode* pNode = apRef->GetNiNode(); pNode && aMotion.HasShownAt)
        aMotion.Shown.Add(aMotion.ShownAt, WorldPoseOf(pNode), false);
    spdlog::debug("HeldObject: {:X} flew here after a throw over there; following the mover's updates again", apRef->formID);
    return cStep;
}

void DroppedItemService::LaunchHere(TESObjectREFR* apRef, const Motion& acMotion, const char* acpKind, const uint32_t aId) noexcept
{
    const HeldObject::Flight& cFlight = acMotion.RemoteThrow.GetFlight();
    apRef->SetMotionType(TESObjectREFR::kMotionDynamic);
    PropHavok::Launch(apRef->formID, cFlight.Velocity, cFlight.Spin);
    spdlog::debug("{}: {:X} was thrown over there; flies here at {:.0f} units a second", acpKind, aId, glm::length(cFlight.Velocity));
}

void DroppedItemService::CatchHere(const TESObjectREFR* apRef, Motion& aMotion, const std::chrono::steady_clock::time_point aNow, const char* acpKind, const uint32_t aId) noexcept
{
    // A carry here from now on: it is sent as a start once it moves.
    PropHavok::CancelLaunch(apRef->formID);
    aMotion.Held = false;
    aMotion.QuietUntil = {};
    aMotion.StopShowing();
    aMotion.RemoteThrow = {};
    aMotion.Moving = false;
    aMotion.Blast = false;
    aMotion.LastLocalMove = aNow;
    aMotion.HeldMoves = 0;
    aMotion.HeldRebuilds = 0;
    spdlog::debug("{}: {:X} was caught here in its flight after a throw over there; moving it from here", acpKind, aId);
}

void DroppedItemService::ReadHolds() noexcept
{
#ifdef SKYRIMVR
    const HeldObject::Carry::Hands cBefore = m_holds.Hands;
    m_holds = {};
    for (uint8_t side = 0; side < 2; ++side)
    {
        HeldObject::Carry::Hand& hand = m_holds.Hands[side];
        hand.Has = VRBodySync::LocalHandPosition(side, hand.Position);
        if (hand.Has && cBefore[side].Has)
            hand.Step = hand.Position - cBefore[side].Position;
    }
#else
    m_holds = {};
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (const TESObjectREFR* pHeld = pPlayer && pPlayer->grabbedObjectHandle ? TESObjectREFR::GetByHandle(pPlayer->grabbedObjectHandle) : nullptr)
        m_holds.GrabbedFormId = pHeld->formID;
#endif
}

bool DroppedItemService::PollHeld(const TESObjectREFR* apRef, Motion& aMotion, const glm::vec3& acPosition) noexcept
{
    const bool cCarried = aMotion.OwnCarry.Poll(m_holds.Hands, acPosition);
    return cCarried || (m_holds.GrabbedFormId && apRef->formID == m_holds.GrabbedFormId);
}

void DroppedItemService::NoteFlight(
    TESObjectREFR* apRef, Motion& aMotion, const glm::vec3& acPosition, const glm::mat3& acTurn, const double aNow, const bool aHeld, const bool aMoving) noexcept
{
    const HeldObject::Pose cPose{acPosition, glm::normalize(glm::quat_cast(acTurn))};
    // Only a moving object can fly: one at rest has its release cleared, and its body is not read.
    if (aMotion.OwnRelease.Poll(aNow, aHeld, aMoving))
    {
        // Its body's own speed when it can be read; otherwise from the last two polls.
        HeldObject::Flight flight{};
        if (!PropHavok::ReadFlight(apRef, flight.Velocity, flight.Spin))
            flight = HeldObject::FlightBetween(aMotion.PolledPose, cPose, aNow - aMotion.PolledAt);
        aMotion.OwnFlight = HeldObject::Capped(flight);
    }
    aMotion.PolledPose = cPose;
    aMotion.PolledAt = aNow;
}
