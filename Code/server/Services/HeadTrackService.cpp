#include <Services/HeadTrackService.h>
#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Messages/RequestNpcHeadTrack.h>
#include <Messages/NotifyNpcHeadTrack.h>

#include <EpochDrop.h>
#include <NpcHeadTrack.h>

HeadTrackService::HeadTrackService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_npcHeadTrackConnection = aDispatcher.sink<PacketEvent<RequestNpcHeadTrack>>().connect<&HeadTrackService::OnNpcHeadTrack>(this);
}

// The owner's words go to every other player near the NPC, as SendToPlayersInRange has it: ownership is server-wide.
// The words for one player are sent together.
void HeadTrackService::OnNpcHeadTrack(const PacketEvent<RequestNpcHeadTrack>& acMessage) noexcept
{
    Player* pSender = acMessage.pPlayer;
    if (!pSender)
        return;

    const auto cNow = std::chrono::steady_clock::now();
    TiltedPhoques::Map<Player*, NotifyNpcHeadTrack> notifies;
    for (const NpcHeadTrackWord& cWord : acMessage.Packet.Words)
    {
        const entt::entity cEntity = static_cast<entt::entity>(cWord.NpcId);
        if (!m_world.valid(cEntity))
            continue;

        const auto* pCharacter = m_world.try_get<CharacterComponent>(cEntity);
        const auto* pOwner = m_world.try_get<OwnerComponent>(cEntity);
        const auto* pLook = m_world.try_get<HeadTrackComponent>(cEntity);
        const NpcHeadTrack::Look cWhat{cWord.Flags, {cWord.X, cWord.Y, cWord.Z}};
        const bool cChanged = !pLook || pLook->Relayed.Flags != cWhat.Flags || pLook->Relayed.At.X != cWhat.At.X || pLook->Relayed.At.Y != cWhat.At.Y ||
                              pLook->Relayed.At.Z != cWhat.At.Z || pLook->RelayedEpoch != cWord.OwnershipEpoch;
        const uint64_t cSinceRelayed = pLook && pLook->RelayedAt.time_since_epoch().count()
                                           ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(cNow - pLook->RelayedAt).count())
                                           : std::numeric_limits<uint64_t>::max();

        const NpcHeadTrack::Word cRule{cWord.NpcId, cWhat, cWord.OwnershipEpoch};
        const NpcHeadTrack::ServerVerdict cVerdict = NpcHeadTrack::ServerTakes(
            pCharacter != nullptr, pCharacter && pCharacter->IsPlayer(), pOwner && pOwner->GetOwner() == pSender, pOwner ? pOwner->OwnershipEpoch : 0, cRule, cChanged,
            cSinceRelayed);
        if (cVerdict == NpcHeadTrack::ServerVerdict::kEpochDrop)
            ReportEpochDrop("where an NPC looks", cWord.NpcId, cWord.OwnershipEpoch, pOwner ? pOwner->OwnershipEpoch : 0);
        if (cVerdict != NpcHeadTrack::ServerVerdict::kRelay)
            continue;

        const auto* pCell = m_world.try_get<CellIdComponent>(cEntity);
        if (!pCell)
            continue;

        auto& look = m_world.get_or_emplace<HeadTrackComponent>(cEntity);
        look.Relayed = cWhat;
        look.RelayedEpoch = cWord.OwnershipEpoch;
        look.RelayedAt = cNow;

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
