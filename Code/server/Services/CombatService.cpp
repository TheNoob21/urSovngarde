#include <Services/CombatService.h>
#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Messages/ProjectileLaunchRequest.h>
#include <Messages/NotifyProjectileLaunch.h>
#include <Messages/RequestHitReaction.h>
#include <Messages/NotifyHitReaction.h>
#include <Messages/RequestDetectionLevels.h>
#include <Messages/NotifyDetectionLevels.h>
#include <Messages/RequestCrimeReport.h>
#include <Messages/NotifyCrimeReport.h>
#include <Messages/RequestCombatTarget.h>
#include <Messages/NotifyCombatTarget.h>

#include <ClientTrust.h>
#include <ClientTrustFacts.h>
#include <CrimeReports.h>

#include <algorithm>
#include <CombatTargets.h>
#include <DetectionLevels.h>
#include <EpochDrop.h>
#include <HitReactions.h>

CombatService::CombatService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_projectileLaunchConnection = aDispatcher.sink<PacketEvent<ProjectileLaunchRequest>>().connect<&CombatService::OnProjectileLaunchRequest>(this);
    m_hitReactionConnection = aDispatcher.sink<PacketEvent<RequestHitReaction>>().connect<&CombatService::OnHitReactionRequest>(this);
    m_detectionConnection = aDispatcher.sink<PacketEvent<RequestDetectionLevels>>().connect<&CombatService::OnDetectionLevels>(this);
    m_crimeReportConnection = aDispatcher.sink<PacketEvent<RequestCrimeReport>>().connect<&CombatService::OnCrimeReport>(this);
    m_combatTargetConnection = aDispatcher.sink<PacketEvent<RequestCombatTarget>>().connect<&CombatService::OnCombatTargets>(this);
}

void CombatService::OnProjectileLaunchRequest(const PacketEvent<ProjectileLaunchRequest>& acMessage) const noexcept
{
    auto& packet = acMessage.Packet;

    // Only the game that runs the shooter says what it shot, from where the shooter is.
    if (GameServer::Get()->ValidatesClientReports())
    {
        const auto cShooter = ClientTrust::FactsOf(m_world, packet.ShooterID, acMessage.pPlayer);
        float originDistance = -1.f;
        if (const auto* pMovement = cShooter.Known ? m_world.try_get<MovementComponent>(static_cast<entt::entity>(packet.ShooterID)) : nullptr; pMovement && pMovement->Tick)
        {
            const ClientTrust::Position cFrom{pMovement->Position.x, pMovement->Position.y, pMovement->Position.z};
            originDistance = ClientTrust::Distance(cFrom, ClientTrust::Position{packet.OriginX, packet.OriginY, packet.OriginZ});
        }
        if (const auto cRefusal =
                ClientTrust::JudgeShot(cShooter.Known, cShooter.SenderOwns, originDistance, packet.OriginX, packet.OriginY, packet.OriginZ, packet.Power, packet.Scale);
            cRefusal != ClientTrust::Refusal::kNone)
        {
            spdlog::debug("Refused a shot of {:X} from player {}: {}", packet.ShooterID, acMessage.pPlayer ? acMessage.pPlayer->GetId() : 0, ClientTrust::RefusalName(cRefusal));
            return;
        }
    }

    NotifyProjectileLaunch notify{};

    notify.ShooterID = packet.ShooterID;

    notify.OriginX = packet.OriginX;
    notify.OriginY = packet.OriginY;
    notify.OriginZ = packet.OriginZ;

    notify.ProjectileBaseID = packet.ProjectileBaseID;
    notify.WeaponID = packet.WeaponID;
    notify.AmmoID = packet.AmmoID;

    notify.ZAngle = packet.ZAngle;
    notify.XAngle = packet.XAngle;
    notify.YAngle = packet.YAngle;

    notify.ParentCellID = packet.ParentCellID;

    notify.SpellID = packet.SpellID;
    notify.CastingSource = packet.CastingSource;

    notify.Area = packet.Area;
    notify.Power = packet.Power;
    notify.Scale = packet.Scale;

    notify.AlwaysHit = packet.AlwaysHit;
    notify.NoDamageOutsideCombat = packet.NoDamageOutsideCombat;
    notify.AutoAim = packet.AutoAim;
    notify.DeferInitialization = packet.DeferInitialization;
    notify.ForceConeOfFire = packet.ForceConeOfFire;

    notify.UnkBool1 = packet.UnkBool1;
    notify.UnkBool2 = packet.UnkBool2;

    const auto cShooterEntity = static_cast<entt::entity>(packet.ShooterID);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cShooterEntity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void CombatService::OnHitReactionRequest(const PacketEvent<RequestHitReaction>& acMessage) noexcept
{
    const auto& packet = acMessage.Packet;
    const entt::entity cEntity = static_cast<entt::entity>(packet.Id);

    float magnitude = packet.Magnitude;
    float direction = packet.Direction;
    if (!HitReactions::Sanitize(packet.Kind, magnitude, direction))
        return;

    const auto* pCharacter = m_world.try_get<CharacterComponent>(cEntity);
    const auto* pOwner = m_world.try_get<OwnerComponent>(cEntity);
    const auto* pCell = m_world.try_get<CellIdComponent>(cEntity);
    if (!acMessage.pPlayer || !HitReactions::ServerTakes(
                                  pCharacter != nullptr, pCharacter && pCharacter->IsPlayer(), pOwner != nullptr, pOwner && pOwner->GetOwner() == acMessage.pPlayer,
                                  pCell && pCell->IsInRange(acMessage.pPlayer->GetCellComponent(), false)))
        return;

    if (m_reactionCounts.size() > 1024)
        m_reactionCounts.clear();
    if (!m_reactionCounts[static_cast<uint64_t>(packet.Id) << 32 | acMessage.pPlayer->GetId()].Take(std::chrono::steady_clock::now()))
        return;

    NotifyHitReaction notify;
    notify.Id = packet.Id;
    notify.Kind = packet.Kind;
    notify.Magnitude = magnitude;
    notify.Direction = direction;

    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

// The owner's words go each to the one player they are about: only that game's copies of the NPC need them.
void CombatService::OnDetectionLevels(const PacketEvent<RequestDetectionLevels>& acMessage) noexcept
{
    Player* pSender = acMessage.pPlayer;
    if (!pSender || !pSender->GetParty().JoinedPartyId)
        return;

    auto& playerManager = m_world.GetPlayerManager();
    TiltedPhoques::Map<Player*, NotifyDetectionLevels> notifies;
    for (const DetectionWord& cWord : acMessage.Packet.Words)
    {
        const entt::entity cEntity = static_cast<entt::entity>(cWord.NpcId);
        if (!m_world.valid(cEntity) || cWord.Band >= DetectionLevels::kBandCount)
            continue;

        Player* pSeen = playerManager.GetById(cWord.PlayerId);
        const auto* pCharacter = m_world.try_get<CharacterComponent>(cEntity);
        const auto* pOwner = m_world.try_get<OwnerComponent>(cEntity);
        const auto* pCell = m_world.try_get<CellIdComponent>(cEntity);
        if (!pSeen || pSeen == pSender ||
            !DetectionLevels::ServerTakes(
                pCharacter != nullptr, pCharacter && pCharacter->IsPlayer(), pOwner && pOwner->GetOwner() == pSender,
                pSeen->GetParty().JoinedPartyId == pSender->GetParty().JoinedPartyId, pCell && pCell->IsInRange(pSeen->GetCellComponent(), false)))
            continue;

        notifies[pSeen].Words.push_back(cWord);
    }

    for (auto& [pSeen, notify] : notifies)
        pSeen->Send(notify);
}

// A crime goes to the game that runs each NPC that saw it, with only that game's own witnesses; nobody else hears of it.
// The sender is the offender: no client names another.
void CombatService::OnCrimeReport(const PacketEvent<RequestCrimeReport>& acMessage) noexcept
{
    Player* pSender = acMessage.pPlayer;
    const CrimeReport& cReport = acMessage.Packet.Report;
    if (!pSender || !pSender->GetParty().JoinedPartyId || !CrimeReports::Taken(cReport.Kind))
        return;

    if (m_crimeReportCounts.size() > 1024)
        m_crimeReportCounts.clear();
    if (!m_crimeReportCounts[pSender->GetId()].Take(std::chrono::steady_clock::now()))
        return;

    TiltedPhoques::Map<Player*, NotifyCrimeReport> notifies;
    for (size_t i = 0; i < cReport.Witnesses.size(); ++i)
    {
        const uint32_t cWitness = cReport.Witnesses[i];
        const entt::entity cEntity = static_cast<entt::entity>(cWitness);
        // Each witness once, however often a client lists it.
        if (!m_world.valid(cEntity) || std::find(cReport.Witnesses.begin(), cReport.Witnesses.begin() + i, cWitness) != cReport.Witnesses.begin() + i)
            continue;

        const auto* pCharacter = m_world.try_get<CharacterComponent>(cEntity);
        const auto* pOwner = m_world.try_get<OwnerComponent>(cEntity);
        const auto* pCell = m_world.try_get<CellIdComponent>(cEntity);
        Player* pWitnessOwner = pOwner ? pOwner->GetOwner() : nullptr;
        if (!CrimeReports::ServerTakesWitness(
                pCharacter != nullptr, pCharacter && pCharacter->IsPlayer(), pWitnessOwner != nullptr, pWitnessOwner == pSender,
                pWitnessOwner && pWitnessOwner->GetParty().JoinedPartyId == pSender->GetParty().JoinedPartyId, pCell && pCell->IsInRange(pSender->GetCellComponent(), false)))
            continue;

        m_world.GetCharacterService().NoteCrimeWitness(cEntity, pSender->GetId(), pOwner->OwnershipEpoch);
        NotifyCrimeReport& notify = notifies[pWitnessOwner];
        if (notify.Report.Witnesses.empty())
        {
            notify.OffenderPlayerId = pSender->GetId();
            notify.Report = cReport;
            notify.Report.Witnesses.clear();
        }
        notify.Report.Witnesses.push_back(cWitness);
    }

    for (auto& [pWitnessOwner, notify] : notifies)
        pWitnessOwner->Send(notify);
}

// The owner's words go to every other player near the NPC, as SendToPlayersInRange has it: ownership is server-wide, so no party is asked for.
// The words for one player are sent together.
void CombatService::OnCombatTargets(const PacketEvent<RequestCombatTarget>& acMessage) noexcept
{
    Player* pSender = acMessage.pPlayer;
    if (!pSender)
        return;

    const auto cNow = std::chrono::steady_clock::now();
    TiltedPhoques::Map<Player*, NotifyCombatTarget> notifies;
    for (const CombatTargetWord& cWord : acMessage.Packet.Words)
    {
        const entt::entity cEntity = static_cast<entt::entity>(cWord.NpcId);
        if (!m_world.valid(cEntity))
            continue;

        const entt::entity cTarget = static_cast<entt::entity>(cWord.TargetId);
        const auto* pCharacter = m_world.try_get<CharacterComponent>(cEntity);
        const auto* pOwner = m_world.try_get<OwnerComponent>(cEntity);
        const auto* pPresence = m_world.try_get<PresenceComponent>(cEntity);
        const bool cTargetIsCharacter = cWord.TargetId && m_world.valid(cTarget) && m_world.all_of<CharacterComponent>(cTarget);
        const bool cChanged = !pPresence || pPresence->CombatTargetRelayed != cWord.TargetId || pPresence->CombatTargetRelayedEpoch != cWord.OwnershipEpoch;
        const uint64_t cSinceRelayed = pPresence && pPresence->CombatTargetRelayedAt.time_since_epoch().count()
                                           ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(cNow - pPresence->CombatTargetRelayedAt).count())
                                           : std::numeric_limits<uint64_t>::max();

        const CombatTargets::Word cRule{cWord.NpcId, cWord.TargetId, cWord.OwnershipEpoch};
        const CombatTargets::ServerVerdict cVerdict = CombatTargets::ServerTakes(
            pCharacter != nullptr, pCharacter && pCharacter->IsPlayer(), pOwner && pOwner->GetOwner() == pSender, pOwner ? pOwner->OwnershipEpoch : 0, cRule, cTargetIsCharacter,
            cChanged, cSinceRelayed);
        if (cVerdict == CombatTargets::ServerVerdict::kEpochDrop)
            ReportEpochDrop("a combat target", cWord.NpcId, cWord.OwnershipEpoch, pOwner ? pOwner->OwnershipEpoch : 0);
        if (cVerdict == CombatTargets::ServerVerdict::kSkip || cVerdict == CombatTargets::ServerVerdict::kEpochDrop)
            continue;

        // The hand-off target (PresenceService::FightOf) is then as fresh as this word.
        auto& presence = m_world.get_or_emplace<PresenceComponent>(cEntity);
        presence.CombatTarget = cWord.TargetId;
        presence.CombatTargetAt = cNow;
        if (cVerdict != CombatTargets::ServerVerdict::kRelay)
            continue;

        const auto* pCell = m_world.try_get<CellIdComponent>(cEntity);
        if (!pCell)
            continue;
        presence.CombatTargetRelayed = cWord.TargetId;
        presence.CombatTargetRelayedEpoch = cWord.OwnershipEpoch;
        presence.CombatTargetRelayedAt = cNow;

        const bool cIsDragon = pCharacter->IsDragon();
        for (Player* pPlayer : m_world.GetPlayerManager())
        {
            if (pPlayer != pSender && pCell->IsInRange(pPlayer->GetCellComponent(), cIsDragon))
                notifies[pPlayer].Words.push_back({cWord.NpcId, cWord.TargetId, pOwner->OwnershipEpoch});
        }
    }

    for (auto& [pPlayer, notify] : notifies)
        pPlayer->Send(notify);
}
