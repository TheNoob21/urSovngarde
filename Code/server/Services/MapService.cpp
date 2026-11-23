#include <Services/MapService.h>
#include <GameServer.h>

#include <Messages/NotifyRemoveWaypoint.h>
#include <Messages/NotifySetWaypoint.h>
#include <Messages/RequestRemoveWaypoint.h>
#include <Messages/RequestSetWaypoint.h>
#include <Messages/RequestMapDiscovery.h>
#include <Messages/NotifyMapDiscovery.h>
#include <Services/PartyService.h>
#include <World.h>

MapService::MapService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
      m_playerSetWaypointConnection = 
          aDispatcher.sink<PacketEvent<RequestSetWaypoint>>().connect<&MapService::OnSetWaypointRequest>(this);
      m_playerRemoveWaypointConnection =
          aDispatcher.sink<PacketEvent<RequestRemoveWaypoint>>().connect<&MapService::OnRemoveWaypointRequest>(this);
      m_mapDiscoveryConnection = aDispatcher.sink<PacketEvent<RequestMapDiscovery>>().connect<&MapService::OnMapDiscoveryRequest>(this);
}

void MapService::OnSetWaypointRequest(const PacketEvent<RequestSetWaypoint>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    NotifySetWaypoint notify{};
    notify.Position = message.Position;
    notify.WorldSpaceFormID = message.WorldSpaceFormID;

    PartyService::Party* pParty = m_world.GetPartyService().GetPlayerParty(acMessage.pPlayer);
    if (!pParty || !notify.WorldSpaceFormID)
        return;

    pParty->Waypoint = notify;
    GameServer::Get()->SendToParty(notify, acMessage.pPlayer->GetParty(), acMessage.GetSender());
}

void MapService::OnRemoveWaypointRequest(const PacketEvent<RequestRemoveWaypoint>& acMessage) const noexcept
{
    PartyService::Party* pParty = m_world.GetPartyService().GetPlayerParty(acMessage.pPlayer);
    if (!pParty)
        return;

    pParty->Waypoint.reset();
    GameServer::Get()->SendToParty(NotifyRemoveWaypoint{}, acMessage.pPlayer->GetParty(), acMessage.GetSender());
}

// A member's discovery is the party's: kept for a member who joins later (PartyService::SendPartyJoinedEvent), and sent to the others.
void MapService::OnMapDiscoveryRequest(const PacketEvent<RequestMapDiscovery>& acMessage) noexcept
{
    PartyService::Party* pParty = m_world.GetPartyService().GetPlayerParty(acMessage.pPlayer);
    if (!pParty)
        return;

    NotifyMapDiscovery notify{};
    for (const MapDiscovery& cDiscovery : acMessage.Packet.Discoveries)
    {
        const uint64_t cKey = cDiscovery.MarkerId.LogFormat();
        // The party's whole list must fit the one message a joining member gets.
        if (pParty->Discoveries.Size() >= MapDiscoveryList::kMax)
            break;
        if (cDiscovery.MarkerId != GameId{} && pParty->Discoveries.Add(cKey, cDiscovery.CanTravel))
            notify.Discoveries.push_back(cDiscovery);
    }

    if (notify.Discoveries.empty())
        return;

    spdlog::debug("MapDiscovery: {} marker(s) discovered by player {}; the party has {}", notify.Discoveries.size(), acMessage.pPlayer->GetId(), pParty->Discoveries.Size());
    GameServer::Get()->SendToParty(notify, acMessage.pPlayer->GetParty(), acMessage.GetSender());
}
