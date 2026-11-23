#include <Services/FurnitureService.h>
#include <Services/ExtensionService.h>
#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Events/CharacterRemoveEvent.h>
#include <Events/UpdateEvent.h>
#include <Messages/RequestNpcFurniture.h>
#include <Messages/NotifyNpcFurniture.h>
#include <Messages/RequestPlayerFurniture.h>
#include <Messages/NotifyPlayerFurniture.h>

#include <EpochDrop.h>
#include <NpcFurniture.h>

namespace
{
uint64_t NowMs() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool InSeatCell(const World& acWorld, const entt::entity aEntity, const Player& acOwner, const GameId& acCell) noexcept
{
    const auto* pCell = acWorld.try_get<CellIdComponent>(aEntity);
    const auto* pCharacter = acWorld.try_get<CharacterComponent>(aEntity);
    const auto cOwnerCell = acOwner.GetStandingCell();
    return pCharacter && pCharacter->IsPlayer() && !pCharacter->IsDead() && !acWorld.all_of<RideComponent>(aEntity) && acCell && pCell && pCell->Cell == acCell &&
           cOwnerCell.Cell == acCell && pCell->WorldSpaceId == cOwnerCell.WorldSpaceId;
}

NotifyPlayerFurniture SeatNotice(const PlayerFurniture::Claim& acClaim, const bool aReleased) noexcept
{
    NotifyPlayerFurniture notify;
    notify.Actor = acClaim.Actor;
    notify.Epoch = acClaim.Epoch;
    notify.Lease = acClaim.Lease;
    notify.Where = acClaim.Where;
    notify.Cell = acClaim.Cell;
    if (!aReleased)
        notify.State = acClaim.State == PlayerFurniture::Phase::Sleeping ? PlayerFurniture::SeatState::Sleeping : PlayerFurniture::SeatState::Sitting;
    return notify;
}
} // namespace

FurnitureService::FurnitureService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_npcFurnitureConnection = aDispatcher.sink<PacketEvent<RequestNpcFurniture>>().connect<&FurnitureService::OnNpcFurniture>(this);
    m_playerFurnitureConnection = aDispatcher.sink<PacketEvent<RequestPlayerFurniture>>().connect<&FurnitureService::OnPlayerFurniture>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&FurnitureService::OnUpdate>(this);
    m_characterRemoveConnection = aDispatcher.sink<CharacterRemoveEvent>().connect<&FurnitureService::OnCharacterRemove>(this);
}

bool FurnitureService::IsSeated(uint32_t aActor) const noexcept
{
    if (m_playerSeats.Find(aActor, NowMs()))
        return true;
    const auto entity = static_cast<entt::entity>(aActor);
    const auto* pSeat = m_world.try_get<SeatComponent>(entity);
    const auto* pOwner = m_world.try_get<OwnerComponent>(entity);
    return pOwner && pSeat && pSeat->RelayedEpoch == pOwner->OwnershipEpoch && NpcFurniture::IsSettled(pSeat->Relayed.State) &&
           std::chrono::steady_clock::now() - pSeat->RelayedAt < std::chrono::milliseconds(NpcFurniture::kExpireMs);
}

void FurnitureService::OnPlayerFurniture(const PacketEvent<RequestPlayerFurniture>& acMessage) noexcept
{
    Player* pSender = acMessage.pPlayer;
    const auto cEntity = pSender ? pSender->GetCharacter() : std::nullopt;
    const auto& cRequest = acMessage.Packet.Request;
    if (!cEntity || !m_world.valid(*cEntity) || World::ToInteger(*cEntity) != cRequest.Actor)
        return;

    const auto* pOwner = m_world.try_get<OwnerComponent>(*cEntity);
    if (!pOwner || pOwner->GetOwner() != pSender || pOwner->OwnershipEpoch != cRequest.Epoch)
        return;

    const GameId& cCell = acMessage.Packet.Cell;
    const bool cMayOccupy =
        !m_world.ctx().at<ExtensionService>().Reserved(cRequest.Actor) && InSeatCell(m_world, *cEntity, *pSender, cCell) && pSender->HasModId(cRequest.Where.Reference.ModId);
    const auto cResult = m_playerSeats.Apply(cRequest, {World::ToInteger(*cEntity), pOwner->OwnershipEpoch, cMayOccupy, cCell}, NowMs());
    if (cResult.Released)
        RelayPlayerSeat(*cResult.Released, true);
    if (cResult.Current && (cResult.Decision == PlayerFurniture::Verdict::Settled || cResult.Decision == PlayerFurniture::Verdict::Refreshed))
        RelayPlayerSeat(*cResult.Current, false);

    if (cRequest.Change == PlayerFurniture::Action::Exit || cRequest.Change == PlayerFurniture::Action::Cancel)
        return;

    NotifyPlayerFurniture reply;
    reply.Actor = cRequest.Actor;
    reply.Epoch = pOwner->OwnershipEpoch;
    reply.Sequence = cRequest.Sequence;
    reply.Where = cRequest.Where;
    reply.Cell = cCell;
    reply.Decision = cResult.Decision;
    if (cResult.Current)
        reply.Lease = cResult.Current->Lease;
    pSender->Send(reply);
}

void FurnitureService::RelayPlayerSeat(const PlayerFurniture::Claim& acClaim, const bool aReleased) const noexcept
{
    const auto cEntity = static_cast<entt::entity>(acClaim.Actor);
    const auto* pOwner = m_world.valid(cEntity) ? m_world.try_get<OwnerComponent>(cEntity) : nullptr;
    if (pOwner && acClaim.State != PlayerFurniture::Phase::Entering)
        GameServer::Get()->SendToPlayersInRange(SeatNotice(acClaim, aReleased), cEntity, pOwner->GetOwner());
}

void FurnitureService::SendPlayerSeat(Player* apViewer, const entt::entity aEntity) const noexcept
{
    const auto* pClaim = m_playerSeats.Find(World::ToInteger(aEntity), NowMs());
    if (!pClaim || pClaim->State == PlayerFurniture::Phase::Entering)
        return;

    const auto* pOwner = m_world.try_get<OwnerComponent>(aEntity);
    const Player* pPlayer = pOwner ? pOwner->GetOwner() : nullptr;
    if (pPlayer && pPlayer != apViewer && pOwner->OwnershipEpoch == pClaim->Epoch && InSeatCell(m_world, aEntity, *pPlayer, pClaim->Cell))
        apViewer->Send(SeatNotice(*pClaim, false));
}

void FurnitureService::OnUpdate(const UpdateEvent&) noexcept
{
    const uint64_t cNow = NowMs();
    if (cNow < m_nextSeatCheck)
        return;
    m_nextSeatCheck = cNow + NpcFurniture::kReadEveryMs;

    for (const auto& cExpired : m_playerSeats.Expire(cNow))
        RelayPlayerSeat(cExpired, true);
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        const auto cEntity = pPlayer->GetCharacter();
        if (!cEntity || !m_world.valid(*cEntity))
            continue;
        const uint32_t cActor = World::ToInteger(*cEntity);
        const auto* pClaim = m_playerSeats.Find(cActor, cNow);
        if (pClaim && !InSeatCell(m_world, *cEntity, *pPlayer, pClaim->Cell))
            if (const auto cReleased = m_playerSeats.ReleaseActor(cActor))
                RelayPlayerSeat(*cReleased, true);
    }
}

void FurnitureService::OnCharacterRemove(const CharacterRemoveEvent& acEvent) noexcept
{
    m_playerSeats.RemoveActor(acEvent.ServerId);
}

// The owner's words go to every other player near the NPC, as SendToPlayersInRange has it: ownership is server-wide.
// The words for one player are sent together.
void FurnitureService::OnNpcFurniture(const PacketEvent<RequestNpcFurniture>& acMessage) noexcept
{
    Player* pSender = acMessage.pPlayer;
    if (!pSender)
        return;

    const auto cNow = std::chrono::steady_clock::now();
    TiltedPhoques::Map<Player*, NotifyNpcFurniture> notifies;
    for (const NpcFurnitureWord& cWord : acMessage.Packet.Words)
    {
        const entt::entity cEntity = static_cast<entt::entity>(cWord.NpcId);
        if (!m_world.valid(cEntity))
            continue;

        const auto* pCharacter = m_world.try_get<CharacterComponent>(cEntity);
        const auto* pOwner = m_world.try_get<OwnerComponent>(cEntity);
        const auto* pSeat = m_world.try_get<SeatComponent>(cEntity);
        const NpcFurniture::Seat cWhere{cWord.FurnitureModId, cWord.FurnitureBaseId, cWord.Marker, cWord.State, cWord.Placement};
        if (NpcFurniture::IsSettled(cWord.State) && m_world.ctx().at<ExtensionService>().Reserved(cWord.NpcId))
            continue;
        const bool cChanged = !pSeat || !(pSeat->Relayed == cWhere) || pSeat->RelayedEpoch != cWord.OwnershipEpoch;
        const uint64_t cSinceRelayed = pSeat && pSeat->RelayedAt.time_since_epoch().count()
                                           ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(cNow - pSeat->RelayedAt).count())
                                           : std::numeric_limits<uint64_t>::max();

        const NpcFurniture::Word cRule{cWord.NpcId, cWhere, cWord.OwnershipEpoch};
        const NpcFurniture::ServerVerdict cVerdict = NpcFurniture::ServerTakes(
            pCharacter != nullptr, pCharacter && pCharacter->IsPlayer(), pOwner && pOwner->GetOwner() == pSender, pOwner ? pOwner->OwnershipEpoch : 0, cRule, cChanged,
            cSinceRelayed);
        if (cVerdict == NpcFurniture::ServerVerdict::kEpochDrop)
            ReportEpochDrop("an NPC's furniture", cWord.NpcId, cWord.OwnershipEpoch, pOwner ? pOwner->OwnershipEpoch : 0);
        if (cVerdict != NpcFurniture::ServerVerdict::kRelay)
            continue;

        const auto* pCell = m_world.try_get<CellIdComponent>(cEntity);
        if (!pCell)
            continue;

        auto& seat = m_world.get_or_emplace<SeatComponent>(cEntity);
        seat.Relayed = cWhere;
        seat.RelayedEpoch = cWord.OwnershipEpoch;
        seat.RelayedAt = cNow;
        spdlog::debug(
            "Furniture: NPC {:X} at epoch {} is {} furniture {:X}:{:X} (marker {})", cWord.NpcId, cWord.OwnershipEpoch, NpcFurniture::IsSettled(cWord.State) ? "in" : "out of",
            cWord.FurnitureModId, cWord.FurnitureBaseId, cWord.Marker);

        const bool cIsDragon = pCharacter->IsDragon();
        for (Player* pPlayer : m_world.GetPlayerManager())
        {
            if (pPlayer != pSender && pCell->IsInRange(pPlayer->GetCellComponent(), cIsDragon))
                notifies[pPlayer].Words.push_back(cWord);
        }
    }

    for (auto& [pPlayer, notify] : notifies)
        pPlayer->Send(notify);
}
