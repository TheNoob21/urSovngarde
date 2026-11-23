#include <Services/PlayerMarkerService.h>

#include <Helpers/PerfScope.h>
#include <World.h>
#include <Components.h>

#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>

#include <Messages/NotifyPlayerJoined.h>
#include <Messages/NotifyPlayerName.h>
#include <Messages/NotifyPlayerLeft.h>
#include <Messages/NotifyPlayerWhereabouts.h>

#include <Services/PapyrusService.h>
#include <Services/PlayerMapRules.h>
#include <Services/TransportService.h>
#include <Services/CellLookup.h>

#include <Actor.h>
#include <PlayerCharacter.h>
#include <ExtraData/ExtraData.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/TESQuest.h>
#include <Games/Overrides.h>
#include <Games/TES.h>
#include <Interface/UI.h>
#include <WhereaboutsRules.h>
#include <SaveLoad.h>
#include <SaveScope.h>
#include <ScopeExit.h>

#include <mutex>

namespace
{
// SkyrimTogether.esp's records, at the object ids Tools/VR/player-markers.py gives them.
constexpr uint32_t kQuestId = 0x005000;
constexpr uint32_t kHoldingCellId = 0x005001;
constexpr uint32_t kMapMarkerFirst = 0x005040;

// While not connected, the world map's list is checked this often for markers a save left on it.

// A put-back on the map's list is no more often than this per marker (the server tells more often while players move).
constexpr auto kShowOnMapEvery = std::chrono::seconds(2);

// How often the markers near this game's player are checked (KeepMarkersAway): a rider covers about 100 units in that time.
constexpr auto kKeepAwayEvery = std::chrono::milliseconds(100);

// The world map icon for other players (game marker type 11: a standing stone).
constexpr uint16_t kMapMarkerType = 11;

// The ExtraMapMarker (0x2C) data on a marker reference: name, map flags, icon.
struct MapMarkerData
{
    void* pFullNameVTable;
    BSFixedString Name;
    uint8_t Flags; // 1 visible on the map, 2 can travel to
    uint8_t Pad;
    uint16_t Type;
};

struct MapMarkerExtra : BSExtraData
{
    MapMarkerData* pData;
};

constexpr auto kExtraMapMarker = static_cast<ExtraDataType>(0x2C);
constexpr uint8_t kMapMarkerVisible = PlayerMapRules::kMarkerVisible;

// How often the service looks whether the world map is open.
constexpr auto kMapLookEvery = std::chrono::milliseconds(250);

// What a save writes of our markers, until it is done (SaveHoldScope).
PlayerMapRules::SaveHold<TESObjectCELL, NiPoint3, PlayerMarkerService::kMapMarkers> s_saveHold;
using SaveMarker = PlayerMapRules::SavedMarker<TESObjectCELL, NiPoint3>;

// Where marker aIndex waits in the holding cell, as Park puts it.
NiPoint3 HomeOf(const size_t aIndex) noexcept
{
    NiPoint3 point;
    point.x = static_cast<float>(aIndex) * 64.f;
    point.y = 0.f;
    point.z = 0.f;
    return point;
}

Actor* FindPlayerActor(World& aWorld, uint32_t aPlayerId) noexcept
{
    auto view = aWorld.view<FormIdComponent, PlayerComponent>();
    for (const auto cEntity : view)
    {
        if (view.get<PlayerComponent>(cEntity).Id == aPlayerId)
            return Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(cEntity).Id));
    }
    return nullptr;
}

// A position the server gave is where this game's player is: the same room, or the same worldspace outdoors.
// Only then are the two positions comparable.
bool InLocalPlayerSpace(World& aWorld, const GameId& acWorldSpaceId, const GameId& acCellId) noexcept
{
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return false;
    auto& modSystem = aWorld.GetModSystem();
    const TESWorldSpace* pWorldSpace = pPlayer->GetWorldSpace();
    return pWorldSpace ? acWorldSpaceId && modSystem.GetGameId(acWorldSpaceId) == pWorldSpace->formID
                       : pPlayer->parentCell && acCellId && modSystem.GetGameId(acCellId) == pPlayer->parentCell->formID;
}

glm::vec3 LocalPlayerPosition() noexcept
{
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    return pPlayer ? glm::vec3(pPlayer->position.x, pPlayer->position.y, pPlayer->position.z) : glm::vec3{};
}

// The game's own pass over the map's list (AE 40750) can make a marker within its reveal distance (iMapMarkerRevealDistance, 1000 by default;
// the running value is not read here) "discovered" and a fast travel destination, so a marker near the player stands off toward its player.
// That keeps a margin for ordinary movement; a hitch, a teleport or a changed reveal distance can still cross it before the next update.
glm::vec3 MarkerSpotFor(const glm::vec3& acPlayerAt, const bool aNearSpace) noexcept
{
    return aNearSpace ? PlayerMapRules::MarkerSpot(LocalPlayerPosition(), acPlayerAt) : acPlayerAt;
}

NiPoint3 ToNiPoint(const glm::vec3& acPosition) noexcept
{
    NiPoint3 point;
    point.x = acPosition.x;
    point.y = acPosition.y;
    point.z = acPosition.z;
    return point;
}

// "<playerName>, near Whiterun", or the name alone when no place is known.
String MarkerText(const String& acName, const String& acPlace) noexcept
{
    return acPlace.empty() ? acName : acName + ", near " + acPlace;
}
} // namespace

PlayerMarkerService::PlayerMarkerService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_connectedConnection = aDispatcher.sink<ConnectedEvent>().connect<&PlayerMarkerService::OnConnected>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&PlayerMarkerService::OnDisconnected>(this);
    m_joinedConnection = aDispatcher.sink<NotifyPlayerJoined>().connect<&PlayerMarkerService::OnPlayerJoined>(this);
    m_nameConnection = aDispatcher.sink<NotifyPlayerName>().connect<&PlayerMarkerService::OnPlayerName>(this);
    m_leftConnection = aDispatcher.sink<NotifyPlayerLeft>().connect<&PlayerMarkerService::OnPlayerLeft>(this);
    m_whereaboutsConnection = aDispatcher.sink<NotifyPlayerWhereabouts>().connect<&PlayerMarkerService::OnWhereabouts>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&PlayerMarkerService::OnUpdate>(this);

    EventDispatcherManager::Get()->loadGameEvent.RegisterSink(this);
}

bool PlayerMarkerService::FindForms() noexcept
{
    if (m_formsLooked)
        return m_formsFound;
    m_formsLooked = true;

    Mod* pPlugin = ModManager::Get()->GetByName("SkyrimTogether.esp");
    if (!pPlugin)
    {
        spdlog::warn("Player markers: SkyrimTogether.esp is not loaded; no players on the map");
        return false;
    }
    m_modBase = static_cast<uint32_t>(pPlugin->standardId) << 24;

    m_pQuest = Cast<TESQuest>(TESForm::GetById(m_modBase | kQuestId));
    m_pHoldingCell = Cast<TESObjectCELL>(TESForm::GetById(m_modBase | kHoldingCellId));
    bool allFound = m_pQuest && m_pHoldingCell;
    for (size_t i = 0; i < kMapMarkers; ++i)
    {
        m_mapSlots[i].pMarker = Cast<TESObjectREFR>(TESForm::GetById(m_modBase | (kMapMarkerFirst + static_cast<uint32_t>(i))));
        allFound = allFound && m_mapSlots[i].pMarker;
    }
    if (!allFound)
    {
        spdlog::warn("Player markers: SkyrimTogether.esp has no player marker records (quest {}, holding cell {}); an older plugin? No players on the map",
                     m_pQuest ? "found" : "missing", m_pHoldingCell ? "found" : "missing");
        return false;
    }

    auto& papyrus = m_world.ctx().at<PapyrusService>();
    if (!papyrus.Get("Quest", "Stop"))
    {
        spdlog::warn("Player markers: the game's Quest.Stop was not found; no players on the map");
        return false;
    }

    // What a save masks, by the references themselves: a save does not take the lock the markers are moved under.
    std::array<SaveMarker, kMapMarkers> saveMarkers{};
    for (size_t i = 0; i < kMapMarkers; ++i)
    {
        TESObjectREFR* pMarker = m_mapSlots[i].pMarker;
        auto* pExtra = static_cast<MapMarkerExtra*>(pMarker->extraData.GetByType(kExtraMapMarker));
        saveMarkers[i] = {&pMarker->parentCell, &pMarker->position, pExtra && pExtra->pData ? &pExtra->pData->Flags : nullptr, HomeOf(i)};
    }
    s_saveHold.Publish(saveMarkers, m_pHoldingCell);

    spdlog::info("Player markers: forms found in SkyrimTogether.esp (quest {:X})", m_pQuest->formID);
    m_formsFound = true;
    return true;
}

void PlayerMarkerService::OnConnected(const ConnectedEvent&) noexcept
{
    m_connected = true;
}

void PlayerMarkerService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    std::scoped_lock lock(m_markersLock);
    m_connected = false;
    m_sweepDue = true;
    m_names.clear();
    if (m_formsFound)
    {
        // Not while a save writes them as at home: the next update frees them.
        if (InSave())
            m_freeDue = true;
        else if (!FreeAll())
            m_freeDue = true;
        StopQuest();
    }
}

void PlayerMarkerService::OnPlayerJoined(const NotifyPlayerJoined& acMessage) noexcept
{
    m_names[acMessage.PlayerId] = acMessage.Username;
}

void PlayerMarkerService::OnPlayerName(const NotifyPlayerName& acMessage) noexcept
{
    m_names[acMessage.PlayerId] = acMessage.Username;
}

void PlayerMarkerService::OnPlayerLeft(const NotifyPlayerLeft& acMessage) noexcept
{
    std::scoped_lock lock(m_markersLock);
    m_names.erase(acMessage.PlayerId);
    // During a save: the next whereabouts, which no longer lists them, frees their marker.
    if (!m_formsFound || InSave())
        return;

    for (size_t i = 0; i < kMapMarkers; ++i)
        if (m_mapSlots[i].PlayerId == acMessage.PlayerId)
            Free(m_mapSlots[i], i);
}

void PlayerMarkerService::OnWhereabouts(const NotifyPlayerWhereabouts& acMessage) noexcept
{
    std::scoped_lock lock(m_markersLock);
    // During a save the markers stay as they are: the next whereabouts comes within two seconds.
    if (!m_connected || InSave() || !FindForms())
        return;

    PerfScope perfScope("PlayerMarkerService::OnWhereabouts");

    // Every player gets a world map icon only: a compass marker would need a quest the journal lists.
    // A "Fellow travellers" quest still running from an older save is stopped here.
    if (!m_questStarted && m_pQuest->getState() == TESQuest::State::Running)
    {
        m_questStarted = true;
        StopQuest();
        spdlog::info("Player markers: quest Fellow travellers was running from the save; stopped (map icons only)");
    }

    const uint32_t cLocalId = m_world.GetTransport().GetLocalPlayerId();
    const auto isListed = [&](uint32_t aPlayerId) {
        for (const auto& entry : acMessage.Players)
            if (entry.PlayerId == aPlayerId)
                return true;
        return false;
    };

    // Slots of players who left are freed first, so their markers can be reused.
    for (size_t i = 0; i < kMapMarkers; ++i)
        if (m_mapSlots[i].PlayerId && !isListed(m_mapSlots[i].PlayerId))
            Free(m_mapSlots[i], i);

    for (const auto& entry : acMessage.Players)
    {
        // A save that started meanwhile, on another thread: the rest wait for the next whereabouts.
        if (InSave())
            return;
        if (entry.PlayerId == cLocalId)
            continue;

        size_t index = kMapMarkers;
        for (size_t i = 0; i < kMapMarkers && index == kMapMarkers; ++i)
            if (m_mapSlots[i].PlayerId == entry.PlayerId)
                index = i;
        for (size_t i = 0; i < kMapMarkers && index == kMapMarkers; ++i)
            if (m_mapSlots[i].PlayerId == 0)
                index = i;
        if (index == kMapMarkers)
            continue; // more players than markers: the rest go unmarked

        Slot& slot = m_mapSlots[index];
        slot.PlayerId = entry.PlayerId;

        const auto cName = m_names.find(entry.PlayerId);
        const String cText = MarkerText(cName != m_names.end() ? cName->second : String("A traveller"), entry.Place);
        if (cText != slot.Text)
        {
            slot.Text = cText;
            SetMapMarkerName(slot.pMarker, cText);
        }

        // Body loaded: follow the body. Otherwise: follow the server's position. Near this game's player the marker stands off
        // toward them.
        if (Actor* pActor = FindPlayerActor(m_world, entry.PlayerId))
        {
            const PlayerCharacter* pPlayer = PlayerCharacter::Get();
            const glm::vec3 cBody(pActor->position.x, pActor->position.y, pActor->position.z);
            // The same room, or the same worldspace outdoors: only then are the two positions comparable.
            const bool cSameSpace = pPlayer && (pPlayer->parentCell == pActor->parentCell ||
                                                (pPlayer->GetWorldSpace() && pPlayer->GetWorldSpace() == pActor->GetWorldSpace()));
            FollowBody(slot, index, pActor, cSameSpace);
        }
        else
        {
            const glm::vec3 cServer(entry.Position.x, entry.Position.y, entry.Position.z);
            const bool cSameSpace = InLocalPlayerSpace(m_world, entry.WorldSpaceId, entry.CellId);
            FollowServerPosition(slot, index, entry.WorldSpaceId, entry.CellId, cServer, cSameSpace);
        }

        // The game empties the map's list at a load, so a shown marker is put back on it here, every two seconds
        if (!slot.Parked && std::chrono::steady_clock::now() >= slot.NextShowOnMap)
        {
            slot.NextShowOnMap = std::chrono::steady_clock::now() + kShowOnMapEvery;
            ShowOnMap(slot);
        }
    }
}

BSTEventResult PlayerMarkerService::OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*)
{
    m_sweepDue = true;
    return BSTEventResult::kOk;
}

void PlayerMarkerService::OnUpdate(const UpdateEvent&) noexcept
{
    RefreshMapIfDue();

    std::scoped_lock lock(m_markersLock);
    if (InSave())
        return;
    if (m_freeDue)
        m_freeDue = !FreeAll();
    KeepMarkersAway();
    TakeTravelBack();

    // A save made while connected may hold our markers on the map's list, and where they stood then.
    // So after each load or disconnect they all come off the list; while connected, the next whereabouts puts them back.
    if (!m_sweepDue)
        return;
    // Only once a save is loaded: FindForms gives up for good when it fails, and at the menus it could.
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->parentCell || !FindForms())
        return;
    m_sweepDue = false;

    for (Slot& slot : m_mapSlots)
    {
        HideFromMap(slot);
        // The marker may stand anywhere the save left it, so it is placed again.
        slot.Parked = true;
        slot.CellId = GameId{};
        slot.BodyCellId = 0;
    }
}

void PlayerMarkerService::StopQuest() noexcept
{
    if (!m_questStarted)
        return;

    ScopedQuestOverride _;
    using Quest = TESQuest;
    PAPYRUS_FUNCTION(void, Quest, Stop);
    s_pStop(m_pQuest);
    m_questStarted = false;
    spdlog::info("Player markers: quest Fellow travellers stopped");
}

void PlayerMarkerService::FollowServerPosition(
    Slot& aSlot, size_t aIndex, const GameId& acWorldSpaceId, const GameId& acCellId, const glm::vec3& acPosition, const bool aNearSpace) noexcept
{
    // One stood off is aimed again at each report: this game's player may have moved around it.
    if (!aSlot.Parked && !aSlot.KeptAway && aSlot.BodyCellId == 0 && aSlot.CellId == acCellId &&
        glm::distance(aSlot.Position, acPosition) < WhereaboutsRules::FollowDistance(m_mapWasOpen))
        return;

    const glm::vec3 cSpot = MarkerSpotFor(acPosition, aNearSpace);
    TESObjectCELL* pCell = CellLookup::FindOrLoad(m_world, acCellId, acWorldSpaceId, cSpot);
    if (!pCell)
        return;

    if (!MoveMarker(aIndex, pCell, cSpot))
        return;
    aSlot.KeptAway = cSpot != acPosition;
    m_mapChanged = m_mapChanged || aSlot.OnMap;
    if (aSlot.Parked)
        spdlog::info("Player markers: {:X} follows player {} ({}) to ({:.0f}) in cell {:X}", aSlot.pMarker->formID, aSlot.PlayerId, aSlot.Text.c_str(), acPosition, pCell->formID);
    aSlot.Parked = false;
    aSlot.CellId = acCellId;
    aSlot.BodyCellId = 0;
    aSlot.Position = acPosition;
}

void PlayerMarkerService::FollowBody(Slot& aSlot, const size_t aIndex, Actor* apActor, const bool aNearSpace) noexcept
{
    TESObjectCELL* pCell = apActor->parentCell;
    if (!pCell)
        return;

    const glm::vec3 cBody(apActor->position.x, apActor->position.y, apActor->position.z);
    if (!aSlot.Parked && !aSlot.KeptAway && aSlot.BodyCellId == pCell->formID && glm::distance(aSlot.Position, cBody) < WhereaboutsRules::FollowDistance(m_mapWasOpen))
        return;

    // Stood off toward the body, it stays in the body's cell: the map and the compass read only its position.
    const glm::vec3 cSpot = MarkerSpotFor(cBody, aNearSpace);
    if (!MoveMarker(aIndex, pCell, cSpot))
        return;
    aSlot.KeptAway = cSpot != cBody;
    m_mapChanged = m_mapChanged || aSlot.OnMap;
    if (aSlot.Parked)
        spdlog::info("Player markers: {:X} follows player {} ({}), whose body is here, to ({:.0f})", aSlot.pMarker->formID, aSlot.PlayerId, aSlot.Text.c_str(), cBody);
    aSlot.Parked = false;
    aSlot.CellId = GameId{};
    aSlot.BodyCellId = pCell->formID;
    aSlot.Position = cBody;
}

// The game's pass made one of ours a fast travel destination: shown only again (PlayerMapRules::TakeTravelBack).
static bool TakeTravelBackFrom(TESObjectREFR* apMarker) noexcept
{
    auto* pExtra = apMarker ? static_cast<MapMarkerExtra*>(apMarker->extraData.GetByType(kExtraMapMarker)) : nullptr;
    if (!pExtra || !pExtra->pData || !PlayerMapRules::TakeTravelBack(pExtra->pData->Flags))
        return false;
    spdlog::debug("Player markers: {:X} was made a fast travel destination; shown only again", apMarker->formID);
    return true;
}

void PlayerMarkerService::TakeTravelBack() noexcept
{
    if (!m_formsFound)
        return;
    for (const Slot& slot : m_mapSlots)
        TakeTravelBackFrom(slot.pMarker);
}

void PlayerMarkerService::KeepMarkersAway() noexcept
{
    if (!m_formsFound || !m_connected)
        return;
    const auto cNow = std::chrono::steady_clock::now();
    if (cNow < m_nextKeepAway)
        return;
    m_nextKeepAway = cNow + kKeepAwayEvery;

    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->parentCell)
        return;
    const glm::vec3 cPlayerAt = LocalPlayerPosition();
    for (size_t i = 0; i < kMapMarkers; ++i)
    {
        Slot& slot = m_mapSlots[i];
        TESObjectREFR* pMarker = slot.pMarker;
        if (!pMarker || slot.Parked || !slot.PlayerId || !pMarker->parentCell)
            continue;
        const bool cSameSpace = pMarker->parentCell == pPlayer->parentCell || (pPlayer->GetWorldSpace() && pPlayer->GetWorldSpace() == pMarker->GetWorldSpace());
        const glm::vec3 cMarkerAt(pMarker->position.x, pMarker->position.y, pMarker->position.z);
        if (!cSameSpace || glm::distance(cMarkerAt, cPlayerAt) >= PlayerMapRules::kPlaceAgainDistance)
            continue;

        // Toward where its player was last seen; the next whereabouts or body move places it from where they are then.
        const glm::vec3 cSpot = PlayerMapRules::MarkerSpot(cPlayerAt, slot.Position);
        if (!MoveMarker(i, pMarker->parentCell, cSpot))
            return;
        m_mapChanged = m_mapChanged || slot.OnMap;
        if (!slot.KeptAway)
            spdlog::debug("Player markers: {:X} stands off toward player {}, who is near", pMarker->formID, slot.PlayerId);
        slot.KeptAway = true;
    }
}

bool PlayerMarkerService::InSave() const noexcept
{
    return s_saveHold.InSave();
}

bool PlayerMarkerService::MoveMarker(const size_t aIndex, TESObjectCELL* apCell, const glm::vec3& acSpot) noexcept
{
    if (!s_saveHold.BeginMove(aIndex))
        return false;
    const ScopeExit cMoved([]() { s_saveHold.EndMove(); });
    m_mapSlots[aIndex].pMarker->MoveTo(apCell, ToNiPoint(acSpot));
    return true;
}

// Save_Impl(device, output stats, file name), the one call behind every save (CommonLibSSE-NG BGSSaveLoadManager::Save).
TP_THIS_FUNCTION(TSaveLoadManagerSave, bool, BGSSaveLoadManager, int32_t aDeviceId, uint32_t aOutputStats, const char* apFileName);
static TSaveLoadManagerSave* RealSaveLoadManagerSave = nullptr;

namespace
{
// A move under way when a save starts is waited for this long; a move held up in the game behind the save never lands before it.
constexpr auto kSaveWaitsForMove = std::chrono::milliseconds(100);

// The save hook's side of the markers (PlayerMapRules::SaveHold). It needs neither the service nor its lock.
struct SaveHoldScope
{
    void EnterSave() noexcept
    {
        s_saveHold.Enter(kSaveWaitsForMove);
        spdlog::debug("Player markers: {} written to the save as at home in the holding cell", s_saveHold.GetCount());
    }

    void LeaveSave() noexcept
    {
        if (s_saveHold.Leave())
            spdlog::debug("Player markers: back where they stood after the save");
    }
};
} // namespace

static bool TP_MAKE_THISCALL(HookSaveLoadManagerSave, BGSSaveLoadManager, int32_t aDeviceId, uint32_t aOutputStats, const char* apFileName)
{
    SaveHoldScope holdScope;
    const SaveScope::Scoped cScope(&holdScope);
    return TiltedPhoques::ThisCall(RealSaveLoadManagerSave, apThis, aDeviceId, aOutputStats, apFileName);
}

static TiltedPhoques::Initializer s_saveHook(
    []()
    {
        POINTER_SKYRIMSE(TSaveLoadManagerSave, s_save, 35727, 34818);
        if (s_save.Get())
        {
            RealSaveLoadManagerSave = s_save.Get();
            TP_HOOK(&RealSaveLoadManagerSave, HookSaveLoadManagerSave);
        }
        else
            spdlog::warn("Player markers: the save call was not found; a save made while connected keeps other players' markers where they stood");
    });

bool PlayerMarkerService::Park(Slot& aSlot, size_t aIndex) noexcept
{
    if (aSlot.Parked)
        return true;
    // Neither off the map's list nor moved while a save writes them.
    if (s_saveHold.InSave())
        return false;

    HideFromMap(aSlot);
    if (!MoveMarker(aIndex, m_pHoldingCell, glm::vec3(static_cast<float>(aIndex) * 64.f, 0.f, 0.f)))
        return false;
    aSlot.Parked = true;
    aSlot.KeptAway = false;
    spdlog::info("Player markers: {:X} put away (player {})", aSlot.pMarker->formID, aSlot.PlayerId);
    return true;
}

void PlayerMarkerService::Free(Slot& aSlot, size_t aIndex) noexcept
{
    if (!Park(aSlot, aIndex))
        return;
    aSlot.PlayerId = 0;
    aSlot.Text.clear();
    aSlot.CellId = GameId{};
    aSlot.BodyCellId = 0;
}

bool PlayerMarkerService::FreeAll() noexcept
{
    bool allFreed = true;
    for (size_t i = 0; i < kMapMarkers; ++i)
    {
        if (m_mapSlots[i].PlayerId)
            Free(m_mapSlots[i], i);
        allFreed = allFreed && m_mapSlots[i].PlayerId == 0;
    }
    return allFreed;
}

// The world map draws only the markers on the player's list (PlayerCharacter::currentMapMarkers); moving a marker does not put it there.
void PlayerMarkerService::ShowOnMap(Slot& aSlot) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;
    if (!PlayerCharacter::CanEditMapMarkers())
    {
        static bool s_said = false;
        if (!s_said)
        {
            s_said = true;
            spdlog::warn("Player markers: the lock on the world map's marker list is not known; no players on the map");
        }
        return;
    }

    // Shown only: the flags ask the game's own pass over the list not to make it a fast travel destination.
    if (auto* pExtra = static_cast<MapMarkerExtra*>(aSlot.pMarker->extraData.GetByType(kExtraMapMarker)); pExtra && pExtra->pData)
        pExtra->pData->Flags = kMapMarkerVisible;

    const uint32_t cHandle = aSlot.pMarker->GetHandle().handle.iBits;
    if (!pPlayer->AddMapMarker(cHandle))
        return;
    spdlog::info("Player markers: {:X} {} the world map's list for player {} ({} markers on it)", aSlot.pMarker->formID, aSlot.OnMap ? "back on" : "put on", aSlot.PlayerId,
                 pPlayer->currentMapMarkers.length);
    aSlot.OnMap = true;
    m_mapChanged = true;
}

void PlayerMarkerService::HideFromMap(Slot& aSlot) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !aSlot.pMarker || !PlayerCharacter::CanEditMapMarkers())
        return;
    if (pPlayer->RemoveMapMarker(aSlot.pMarker->GetHandle().handle.iBits))
    {
        spdlog::info("Player markers: {:X} taken off the world map's list{}", aSlot.pMarker->formID, m_connected ? "" : " (left there by a save; not connected)");
        m_mapChanged = true;
    }
    aSlot.OnMap = false;
}

// The map gathers the list into its icons only at some moments: as it opens on SE, as its camera rises through height 10000
// on VR, and on an update that carries no data (MapMenu::ProcessMessage on both). So a marker put on the list after the
// gathering is not drawn, and VR keeps the map's icons from one opening to the next. The map is told to gather again
// as it opens and whenever ours changed while it is open.
void PlayerMarkerService::RefreshMapIfDue() noexcept
{
    if (!m_formsFound)
        return;
    const auto cNow = std::chrono::steady_clock::now();
    if (cNow < m_nextMapLook)
        return;
    m_nextMapLook = cNow + kMapLookEvery;

    static const BSFixedString s_mapMenu("MapMenu");
    UI* pUI = UI::Get();
    const bool cOpen = pUI && pUI->GetMenuOpen(s_mapMenu);
    const bool cDue = PlayerMapRules::RefreshDue(cOpen, m_mapWasOpen, m_mapChanged);
    bool queued = false;
    if (cDue)
    {
        queued = UI::RefreshMapMarkers();
        if (queued)
            spdlog::debug("Player markers: the world map was told to gather its markers again ({})", m_mapWasOpen ? "ours changed" : "it opened");
    }
    // Not queued (the queue not there yet as it opens): owed, and tried again at the next look.
    m_mapChanged = PlayerMapRules::StillOwed(cDue, queued, m_mapChanged);
    m_mapWasOpen = cOpen;
}

void PlayerMarkerService::SetMapMarkerName(TESObjectREFR* apMarker, const String& acText) noexcept
{
    auto* pExtra = static_cast<MapMarkerExtra*>(apMarker->extraData.GetByType(kExtraMapMarker));
    if (!pExtra || !pExtra->pData)
    {
        spdlog::warn("Player markers: map marker {:X} has no map marker data", apMarker->formID);
        return;
    }
    pExtra->pData->Name.Set(acText.c_str());
    pExtra->pData->Flags = kMapMarkerVisible; // on the map, not a fast travel destination
    pExtra->pData->Type = kMapMarkerType;
}
