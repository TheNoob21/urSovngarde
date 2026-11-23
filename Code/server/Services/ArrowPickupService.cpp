#include <Services/ArrowPickupService.h>

#include <GameServer.h>
#include <World.h>
#include <Components.h>
#include <Events/PlayerLeaveEvent.h>

#include <Messages/RequestArrowPickup.h>
#include <Messages/NotifyArrowPickup.h>

#include <chrono>

ArrowPickupService::ArrowPickupService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_pickupConnection(aDispatcher.sink<PacketEvent<RequestArrowPickup>>().connect<&ArrowPickupService::OnArrowPickup>(this))
    , m_leaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&ArrowPickupService::OnPlayerLeave>(this))
{
}

void ArrowPickupService::OnArrowPickup(const PacketEvent<RequestArrowPickup>& acMessage) noexcept
{
    const RequestArrowPickup& cMessage = acMessage.Packet;
    const Player* pPlayer = acMessage.pPlayer;
    if (!pPlayer)
        return;

    ArrowPickup::Check check{};
    check.HasArrow = cMessage.ProjectileBaseId != GameId{};
    check.X = cMessage.X;
    check.Y = cMessage.Y;
    check.Z = cMessage.Z;

    const auto cCharacter = pPlayer->GetCharacter();
    if (cCharacter && m_world.valid(*cCharacter))
    {
        // The player's place and cell as this server last heard them; with no cell either, no pick-up is taken.
        // A character that has sent no movement yet has no place (Tick 0, position zero), as in the dropped-item service.
        const auto* pMovement = m_world.try_get<MovementComponent>(*cCharacter);
        const auto* pCell = m_world.try_get<CellIdComponent>(*cCharacter);
        if (pMovement && pMovement->Tick && pCell && *pCell)
        {
            check.HasPicker = true;
            check.PickerX = pMovement->Position.x;
            check.PickerY = pMovement->Position.y;
            check.PickerZ = pMovement->Position.z;
            check.PickerInterior = pCell->IsInInteriorCell();
            check.SameCell = pCell->Cell == cMessage.CellId;
            check.PickerGridX = pCell->CenterCoords.X;
            check.PickerGridY = pCell->CenterCoords.Y;
        }
    }

    ArrowPickup::Refusal refusal = ArrowPickup::Judge(check);
    if (refusal == ArrowPickup::Refusal::kNone)
    {
        const uint64_t cNowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        if (!m_rates[pPlayer->GetId()].Allow(cNowMs))
            refusal = ArrowPickup::Refusal::kTooOften;
    }
    if (refusal != ArrowPickup::Refusal::kNone)
    {
        spdlog::debug(
            "ArrowPickup: player {}'s pick-up of {:X}:{:X} refused: {}", pPlayer->GetId(), cMessage.ProjectileBaseId.ModId, cMessage.ProjectileBaseId.BaseId,
            ArrowPickup::Describe(refusal));
        return;
    }

    NotifyArrowPickup notify{};
    notify.ProjectileBaseId = cMessage.ProjectileBaseId;
    notify.CellId = cMessage.CellId;
    notify.X = cMessage.X;
    notify.Y = cMessage.Y;
    notify.Z = cMessage.Z;
    if (!GameServer::Get()->SendToPlayersInRange(notify, *cCharacter, acMessage.GetSender()))
        spdlog::debug("ArrowPickup: player {}'s pick-up not relayed: their character has no cell", pPlayer->GetId());
}

void ArrowPickupService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    if (acEvent.pPlayer)
        m_rates.erase(acEvent.pPlayer->GetId());
}
