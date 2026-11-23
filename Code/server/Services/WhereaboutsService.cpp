#include <Services/WhereaboutsService.h>

#include <GameServer.h>
#include <World.h>
#include <Components.h>
#include <Events/UpdateEvent.h>

#include <Messages/NotifyPlayerWhereabouts.h>
#include <WhereaboutsRules.h>

Console::Setting bLivePlayerMap{
    "Gameplay:bLivePlayerMap", "Tells the players where everyone is four times a second while someone moves, so world map icons move smoothly. Off: every two seconds", true};

WhereaboutsService::WhereaboutsService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&WhereaboutsService::OnUpdate>(this))
{
}

void WhereaboutsService::OnUpdate(const UpdateEvent&) noexcept
{
    // Looked at as often as a moving position is told; only sent when something changed, or once the heartbeat is due.
    // Off: every two seconds, whatever changed.
    const bool cLive = bLivePlayerMap;
    const auto cNow = std::chrono::steady_clock::now();
    if (cNow < m_nextLook)
        return;
    m_nextLook = cNow + std::chrono::milliseconds(cLive ? WhereaboutsRules::kMovingEveryMs : WhereaboutsRules::kHeartbeatMs);

    auto& players = m_world.GetPlayerManager();
    if (players.Count() < 2)
        return;

    NotifyPlayerWhereabouts notify{};
    for (Player* pPlayer : players)
    {
        const auto cCharacter = pPlayer->GetCharacter();
        if (!cCharacter)
            continue;

        const auto* pCell = m_world.try_get<CellIdComponent>(*cCharacter);
        const auto* pMovement = m_world.try_get<MovementComponent>(*cCharacter);
        if (!pCell || !pMovement)
            continue;

        NotifyPlayerWhereabouts::Entry entry{};
        entry.PlayerId = pPlayer->GetId();
        entry.WorldSpaceId = pCell->WorldSpaceId;
        entry.CellId = pCell->Cell;
        entry.Position = pMovement->Position;
        entry.Place = pPlayer->GetPlace();
        notify.Players.push_back(std::move(entry));
    }

    if (notify.Players.size() < 2)
        return;

    const auto cSinceLast = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(cNow - m_lastSent).count());
    if (cLive && !WhereaboutsRules::SendDue(cSinceLast, !(notify == m_last)))
        return;
    m_lastSent = cNow;
    m_last = notify;

    // Unreliable: the next one is a quarter of a second away while they move, and a lost one is only a marker that much late.
    for (Player* pPlayer : players)
        pPlayer->SendUnreliable(notify);
}
