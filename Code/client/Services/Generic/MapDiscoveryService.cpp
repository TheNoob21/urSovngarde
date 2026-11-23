#include <Services/MapDiscoveryService.h>

#include <World.h>

#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/PartyJoinedEvent.h>
#include <Events/PartyLeftEvent.h>
#include <Events/UpdateEvent.h>

#include <Messages/NotifyMapDiscovery.h>
#include <Messages/RequestMapDiscovery.h>

#include <Services/PapyrusService.h>
#include <Services/TransportService.h>

#include <PlayerCharacter.h>
#include <ExtraData/ExtraData.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Games/TES.h>
#include <Interface/UI.h>

namespace
{
// Discoveries come as the player walks, so a look this often finds them within a few steps.
constexpr auto kLookEvery = std::chrono::seconds(2);

// Skyrim.esm's MapMarker static, the base of every map marker reference.
constexpr uint32_t kMapMarkerBase = 0x10;

// The ExtraMapMarker (0x2C) data on a marker reference, as PlayerMarkerService reads it.
struct MapMarkerData
{
    void* pFullNameVTable;
    BSFixedString Name;
    uint8_t Flags;
    uint8_t Pad;
    uint16_t Type;
};

struct MapMarkerExtra : BSExtraData
{
    MapMarkerData* pData;
};

constexpr auto kExtraMapMarker = static_cast<ExtraDataType>(0x2C);

MapMarkerData* GetMarkerData(const TESObjectREFR* apRef) noexcept
{
    auto* pExtra = apRef ? static_cast<MapMarkerExtra*>(apRef->extraData.GetByType(kExtraMapMarker)) : nullptr;
    return pExtra ? pExtra->pData : nullptr;
}

bool IsInGame() noexcept
{
    static const BSFixedString s_loadingMenu("Loading Menu");
    static const BSFixedString s_mainMenu("Main Menu");
    const UI* pUI = UI::Get();
    return pUI && PlayerCharacter::Get() && !pUI->GetMenuOpen(s_loadingMenu) && !pUI->GetMenuOpen(s_mainMenu);
}

// The game's own way to put a marker on the map, which the save keeps; it shows no discovery message.
void AddToMap(TESObjectREFR* apMarker, bool aCanTravel) noexcept
{
    using ObjectReference = TESObjectREFR;
    PAPYRUS_FUNCTION(void, ObjectReference, AddToMap, bool);
    s_pAddToMap(apMarker, aCanTravel);
}
} // namespace

MapDiscoveryService::MapDiscoveryService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&MapDiscoveryService::OnUpdate>(this);
    m_connectedConnection = aDispatcher.sink<ConnectedEvent>().connect<&MapDiscoveryService::OnConnected>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&MapDiscoveryService::OnDisconnected>(this);
    m_partyJoinedConnection = aDispatcher.sink<PartyJoinedEvent>().connect<&MapDiscoveryService::OnPartyJoined>(this);
    m_partyLeftConnection = aDispatcher.sink<PartyLeftEvent>().connect<&MapDiscoveryService::OnPartyLeft>(this);
    m_notifyConnection = aDispatcher.sink<NotifyMapDiscovery>().connect<&MapDiscoveryService::OnNotifyMapDiscovery>(this);

    EventDispatcherManager::Get()->loadGameEvent.RegisterSink(this);
}

void MapDiscoveryService::NewTerm() noexcept
{
    m_party.clear();
    m_watch.Reset();
    m_markers.clear();
    m_markersWorldSpace = 0;
    m_addDue = false;
}

void MapDiscoveryService::OnConnected(const ConnectedEvent&) noexcept
{
    NewTerm();
}

void MapDiscoveryService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    NewTerm();
}

void MapDiscoveryService::OnPartyJoined(const PartyJoinedEvent&) noexcept
{
    NewTerm();
}

void MapDiscoveryService::OnPartyLeft(const PartyLeftEvent&) noexcept
{
    NewTerm();
}

// On the game's thread, as the load ends: the update after it learns the loaded map, and puts the party's discoveries back on it.
BSTEventResult MapDiscoveryService::OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*)
{
    m_loaded = true;
    return BSTEventResult::kOk;
}

void MapDiscoveryService::OnNotifyMapDiscovery(const NotifyMapDiscovery& acMessage) noexcept
{
    for (const MapDiscovery& cDiscovery : acMessage.Discoveries)
    {
        const uint32_t cFormId = m_world.GetModSystem().GetGameId(cDiscovery.MarkerId);
        if (!cFormId)
            continue; // from a plugin this game does not have

        bool& canTravel = m_party[cFormId];
        canTravel = canTravel || cDiscovery.CanTravel;
    }
    m_addDue = true;
}

void MapDiscoveryService::OnUpdate(const UpdateEvent&) noexcept
{
    if (m_loaded.exchange(false))
    {
        m_watch.Reset();
        m_markers.clear();
        m_markersWorldSpace = 0;
        m_addDue = !m_party.empty();
    }

    if (!m_transport.IsConnected() || !m_world.GetPartyService().IsInParty() || !IsInGame())
        return;

    if (m_addDue)
    {
        m_addDue = false;
        AddPartyDiscoveries();
    }

    const auto cNow = std::chrono::steady_clock::now();
    if (cNow < m_nextLook)
        return;
    m_nextLook = cNow + kLookEvery;

    LookForDiscoveries();
}

void MapDiscoveryService::CollectMarkers(TESWorldSpace* apWorldSpace) noexcept
{
    m_markers.clear();

    // Looked at again at the next look while the world space has no persistent cell of its own.
    const TESObjectCELL* pCell = apWorldSpace->GetPersistentCell();
    if (!pCell || pCell->worldspace != apWorldSpace)
        return;
    m_markersWorldSpace = apWorldSpace->formID;
    // A party discovery that could not be put on the map before is tried again here.
    m_addDue = !m_party.empty();

    // The other players' markers come from SkyrimTogether.esp, and PlayerMarkerService puts them on the map itself.
    const Mod* pOwnPlugin = ModManager::Get()->GetByName("SkyrimTogether.esp");
    const uint32_t cOwnPrefix = pOwnPlugin && pOwnPlugin->IsLoaded() && !pOwnPlugin->IsLite() ? pOwnPlugin->standardId : 0x100;

    for (const TESObjectREFR* pRef : pCell->GetRefsByFormTypes({FormType::Static}))
    {
        if (pRef->baseForm->formID != kMapMarkerBase || (pRef->formID >> 24) == cOwnPrefix || !GetMarkerData(pRef))
            continue;
        m_markers.push_back(pRef->formID);
    }

    spdlog::debug("MapDiscovery: {} map marker(s) in world space {:X}", m_markers.size(), apWorldSpace->formID);
}

void MapDiscoveryService::LookForDiscoveries() noexcept
{
    // Markers are discovered outdoors; one a quest puts on the map meanwhile is found once this player is out again.
    TESWorldSpace* pWorldSpace = PlayerCharacter::Get()->GetWorldSpace();
    if (!pWorldSpace)
        return;

    if (pWorldSpace->formID != m_markersWorldSpace)
        CollectMarkers(pWorldSpace);

    std::vector<std::pair<uint32_t, uint8_t>> markers;
    markers.reserve(m_markers.size());
    for (const uint32_t cFormId : m_markers)
    {
        if (const MapMarkerData* pData = GetMarkerData(Cast<TESObjectREFR>(TESForm::GetById(cFormId))))
            markers.emplace_back(cFormId, pData->Flags);
    }

    RequestMapDiscovery request{};
    for (const auto& cFound : m_watch.Look(markers))
    {
        MapDiscovery discovery{};
        if (!m_world.GetModSystem().GetServerModId(cFound.FormId, discovery.MarkerId))
            continue;
        discovery.CanTravel = cFound.CanTravel;
        request.Discoveries.push_back(discovery);

        // The party's now, and put back on this map after an older save loads.
        bool& canTravel = m_party[cFound.FormId];
        canTravel = canTravel || cFound.CanTravel;
    }

    if (request.Discoveries.empty())
        return;

    spdlog::debug("MapDiscovery: {} marker(s) discovered here; sent to the party", request.Discoveries.size());
    m_transport.Send(request);
}

void MapDiscoveryService::AddPartyDiscoveries() noexcept
{
    size_t added = 0;
    for (const auto& [cFormId, cCanTravel] : m_party)
    {
        TESObjectREFR* pMarker = Cast<TESObjectREFR>(TESForm::GetById(cFormId));
        const MapMarkerData* pData = GetMarkerData(pMarker);
        if (!pData || !MapDiscoveries::NeedsAdding(pData->Flags, cCanTravel))
            continue;

        AddToMap(pMarker, cCanTravel);
        m_watch.Learned(cFormId, cCanTravel);
        ++added;
    }

    if (added)
        spdlog::debug("MapDiscovery: {} marker(s) the party discovered put on this map", added);
}
