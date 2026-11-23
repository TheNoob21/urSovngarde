#include <Services/CharacterService.h>
#include <Services/ExtensionService.h>
#include <Services/PresenceService.h>
#include <Services/PlayerService.h>
#include <Components.h>
#include <ClientTrust.h>
#include <ClientTrustFacts.h>
#include <GameServer.h>
#include <World.h>

#include <Events/CharacterSpawnedEvent.h>
#include <Events/CharacterExteriorCellChangeEvent.h>
#include <Events/CharacterInteriorCellChangeEvent.h>
#include <Events/PlayerEnterWorldEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/CharacterRemoveEvent.h>
#include <Events/OwnershipTransferEvent.h>

#include <Game/OwnerView.h>
#include <MountSync.h>
#include <GrabHold.h>

#include <Messages/ClashRequest.h>
#include <Messages/NotifyClash.h>
#include <Messages/ArrowNockedRequest.h>
#include <Messages/NotifyArrowNocked.h>
#include <Messages/AssignCharacterRequest.h>
#include <Messages/AssignCharacterResponse.h>
#include <Messages/ServerReferencesMoveRequest.h>
#include <Messages/ClientReferencesMoveRequest.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Services/MagicStateService.h>
#include <Services/FurnitureService.h>
#include <Services/RagdollPoseService.h>
#include <Messages/RequestFactionsChanges.h>
#include <Messages/NotifyFactionsChanges.h>
#include <Messages/NotifyRemoveCharacter.h>
#include <Messages/NotifyReferenceEnable.h>
#include <Messages/RequestOwnershipTransfer.h>
#include <Messages/NotifyOwnershipTransfer.h>
#include <Messages/RequestOwnershipClaim.h>
#include <LeaderAssignmentRules.h>
#include <OwnerSpread.h>
#include <RelayThinning.h>
#include <Messages/RequestFollowerState.h>
#include <Messages/RequestFollowerCommand.h>
#include <Events/PlayerLeaveEvent.h>
#include <Messages/NotifyFollowerCommand.h>
#include <Messages/RequestStuckArrow.h>
#include <Messages/NotifyStuckArrow.h>
#include <Messages/RequestDismember.h>
#include <Messages/NotifyDismember.h>
#include <LimbMask.h>
#include <Messages/MountRequest.h>
#include <Messages/NotifyMount.h>
#include <Messages/NotifyNpcFurniture.h>
#include <SeatCatchUp.h>
#include <Messages/RequestDismount.h>
#include <Messages/NotifyDismount.h>
#include <Messages/NewPackageRequest.h>
#include <Messages/NotifyNewPackage.h>
#include <Messages/RequestRespawn.h>
#include <Messages/NotifyRespawn.h>
#include <Messages/SyncExperienceRequest.h>
#include <Messages/NotifySyncExperience.h>
#include <Messages/DialogueRequest.h>
#include <Messages/NotifyDialogue.h>
#include <Messages/SubtitleRequest.h>
#include <Messages/NotifySubtitle.h>
#include <Messages/NotifyActorTeleport.h>
#include <Messages/RequestActionEvents.h>
#include <Messages/NotifyActionEvents.h>
#include <CrimeReports.h>
#include <MovementPacker.h>

#include <Setting.h>
#include <TwinSpawn.h>
namespace
{
Console::Setting bEnableXpSync{"Gameplay:bEnableXpSync", "Syncs combat XP within the party", true};
Console::Setting bGrabHolds{"Gameplay:bGrabHolds", "A party member who grabs a living NPC lying in its ragdoll runs it for the hold, leader or not", true};
Console::Setting bShareScriptSpawns{
    "Gameplay:bShareScriptSpawns", "One set of the actors a script or scene spawns in every game: a game's own spawn stands for the one another game made first", true};
// Counts the player characters made, so each one's spawn says which of a player's characters is the newer.
uint32_t s_playerSpawnGeneration = 0;
Console::Setting bPartyHorses{"Gameplay:bPartyHorses", "Party members may ride each other's horses (off: no game is given a horse another game runs to ride it)", true};
// Any NPC a report names is noted; it is the stock client that asks for guards only (CrimeReports::OffenderAsksForWitness).
Console::Setting bCrimeGuardHandoff{"Gameplay:bCrimeGuardHandoff", "A guard who saw a player's crime goes to that player's game for the arrest, when it asks", true};
}

CharacterService::CharacterService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&CharacterService::OnUpdate>(this))
    , m_interiorCellChangeEventConnection(aDispatcher.sink<CharacterInteriorCellChangeEvent>().connect<&CharacterService::OnCharacterInteriorCellChange>(this))
    , m_exteriorCellChangeEventConnection(aDispatcher.sink<CharacterExteriorCellChangeEvent>().connect<&CharacterService::OnCharacterExteriorCellChange>(this))
    , m_characterAssignRequestConnection(aDispatcher.sink<PacketEvent<AssignCharacterRequest>>().connect<&CharacterService::OnAssignCharacterRequest>(this))
    , m_transferOwnershipConnection(aDispatcher.sink<PacketEvent<RequestOwnershipTransfer>>().connect<&CharacterService::OnOwnershipTransferRequest>(this))
    , m_ownershipTransferEventConnection(aDispatcher.sink<OwnershipTransferEvent>().connect<&CharacterService::OnOwnershipTransferEvent>(this))
    , m_claimOwnershipConnection(aDispatcher.sink<PacketEvent<RequestOwnershipClaim>>().connect<&CharacterService::OnOwnershipClaimRequest>(this))
    , m_followerStateConnection(aDispatcher.sink<PacketEvent<RequestFollowerState>>().connect<&CharacterService::OnFollowerStateRequest>(this))
    , m_followerCommandConnection(aDispatcher.sink<PacketEvent<RequestFollowerCommand>>().connect<&CharacterService::OnFollowerCommandRequest>(this))
    , m_followerCommandLeaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&CharacterService::OnPlayerLeaveFollowerCommands>(this))
    , m_removeCharacterConnection(aDispatcher.sink<CharacterRemoveEvent>().connect<&CharacterService::OnCharacterRemoveEvent>(this))
    , m_characterSpawnedConnection(aDispatcher.sink<CharacterSpawnedEvent>().connect<&CharacterService::OnCharacterSpawned>(this))
    , m_referenceMovementSnapshotConnection(aDispatcher.sink<PacketEvent<ClientReferencesMoveRequest>>().connect<&CharacterService::OnReferencesMoveRequest>(this))
    , m_factionsChangesConnection(aDispatcher.sink<PacketEvent<RequestFactionsChanges>>().connect<&CharacterService::OnFactionsChanges>(this))
    , m_mountConnection(aDispatcher.sink<PacketEvent<MountRequest>>().connect<&CharacterService::OnMountRequest>(this))
    , m_dismountConnection(aDispatcher.sink<PacketEvent<RequestDismount>>().connect<&CharacterService::OnDismountRequest>(this))
    , m_newPackageConnection(aDispatcher.sink<PacketEvent<NewPackageRequest>>().connect<&CharacterService::OnNewPackageRequest>(this))
    , m_requestRespawnConnection(aDispatcher.sink<PacketEvent<RequestRespawn>>().connect<&CharacterService::OnRequestRespawn>(this))
    , m_clashConnection(aDispatcher.sink<PacketEvent<ClashRequest>>().connect<&CharacterService::OnClashRequest>(this))
    , m_arrowNockedConnection(aDispatcher.sink<PacketEvent<ArrowNockedRequest>>().connect<&CharacterService::OnArrowNockedRequest>(this))
    , m_syncExperienceConnection(aDispatcher.sink<PacketEvent<SyncExperienceRequest>>().connect<&CharacterService::OnSyncExperienceRequest>(this))
    , m_dialogueConnection(aDispatcher.sink<PacketEvent<DialogueRequest>>().connect<&CharacterService::OnDialogueRequest>(this))
    , m_subtitleConnection(aDispatcher.sink<PacketEvent<SubtitleRequest>>().connect<&CharacterService::OnSubtitleRequest>(this))
    , m_actionEventsConnection(aDispatcher.sink<PacketEvent<RequestActionEvents>>().connect<&CharacterService::OnActionEventsRequest>(this))
    , m_stuckArrowConnection(aDispatcher.sink<PacketEvent<RequestStuckArrow>>().connect<&CharacterService::OnStuckArrowRequest>(this))
    , m_dismemberConnection(aDispatcher.sink<PacketEvent<RequestDismember>>().connect<&CharacterService::OnDismemberRequest>(this))
{
}

namespace
{
// The player whose follower this is, while that player is connected; 0 otherwise.
uint32_t ConnectedFollowerPlayer(World& aWorld, const OwnerComponent& acOwner) noexcept
{
    return acOwner.FollowerOfPlayerId && aWorld.GetPlayerManager().GetById(acOwner.FollowerOfPlayerId) ? acOwner.FollowerOfPlayerId : 0;
}

// Other games show a living follower on a copy, so nothing moves their own reference of her.
bool IsLivingFollower(World& aWorld, const entt::entity aEntity) noexcept
{
    const auto* pOwner = aWorld.try_get<OwnerComponent>(aEntity);
    const auto* pCharacter = aWorld.try_get<CharacterComponent>(aEntity);
    return pOwner && pCharacter && !pCharacter->IsDead() && ConnectedFollowerPlayer(aWorld, *pOwner);
}

// How far a player stands from a character, empty when the player is elsewhere.
// An interior is loaded whole, so inside the player's own one every character is near.
std::optional<float> DistanceFrom(World& aWorld, const Player* apPlayer, const CellIdComponent& acCell, const glm::vec3& acPosition) noexcept
{
    const auto& cPlayerCell = apPlayer->GetCellComponent();
    if (cPlayerCell.IsInInteriorCell() || acCell.IsInInteriorCell())
    {
        if (cPlayerCell.IsInInteriorCell() && acCell.IsInInteriorCell() && cPlayerCell.Cell == acCell.Cell)
            return 0.f;
        return std::nullopt;
    }
    if (cPlayerCell.WorldSpaceId != acCell.WorldSpaceId || !apPlayer->GetCharacter())
        return std::nullopt;
    const auto* pMovement = aWorld.try_get<MovementComponent>(*apPlayer->GetCharacter());
    if (!pMovement)
        return std::nullopt;
    return glm::distance(pMovement->Position, acPosition);
}

struct NearestMember
{
    Player* pPlayer{};
    std::optional<float> Distance{};
};

// Whether that member's game may take the character over (OwnerSpread::MayTakeOver).
bool MayTakeOver(World& aWorld, const entt::entity aEntity, const OwnerComponent& acOwner, const Player* apMember, const std::chrono::steady_clock::time_point aNow) noexcept
{
    const auto cKind = aWorld.GetPresenceService().LoadedAs(aEntity, apMember->GetId(), aNow);
    const bool cDeclined = std::find(acOwner.InvalidOwners.begin(), acOwner.InvalidOwners.end(), apMember) != acOwner.InvalidOwners.end();
    return OwnerSpread::MayTakeOver(cKind.has_value(), cKind && *cKind == ActorPresenceKind::StandIn, cDeclined, aNow - apMember->GetLastCellEnterAt());
}

// The party member nearest a character who may take it over, the leader left out: one playing, whose range takes the character in.
NearestMember FindNearestMember(
    World& aWorld, const PartyService::Party& acParty, const entt::entity aEntity, const OwnerComponent& acOwner, const CellIdComponent& acCell, const glm::vec3& acPosition,
    const bool aIsDragon, const std::chrono::steady_clock::time_point aNow) noexcept
{
    NearestMember nearest{};
    for (Player* pMember : acParty.Members)
    {
        if (pMember->GetId() == acParty.LeaderPlayerId || PresenceService::IsSilent(pMember, aNow) || !pMember->GetCellComponent().IsInRange(acCell, aIsDragon) ||
            !MayTakeOver(aWorld, aEntity, acOwner, pMember, aNow))
            continue;
        const auto cDistance = DistanceFrom(aWorld, pMember, acCell, acPosition);
        if (cDistance && (!nearest.Distance || *cDistance < *nearest.Distance))
            nearest = NearestMember{pMember, cDistance};
    }
    return nearest;
}
} // namespace

void CharacterService::Serialize(World& aRegistry, entt::entity aEntity, CharacterSpawnRequest* apSpawnRequest) noexcept
{
    const auto& characterComponent = aRegistry.get<CharacterComponent>(aEntity);

    apSpawnRequest->ServerId = World::ToInteger(aEntity);
    apSpawnRequest->AppearanceBuffer = characterComponent.SaveBuffer;
    apSpawnRequest->ChangeFlags = characterComponent.ChangeFlags;
    apSpawnRequest->FaceTints = characterComponent.FaceTints;
    apSpawnRequest->FactionsContent = characterComponent.FactionsContent;
    apSpawnRequest->IsDead = characterComponent.IsDead();
    apSpawnRequest->RevivedSinceDeath = characterComponent.IsRevivedSinceDeath();
    apSpawnRequest->DismemberedLimbs = characterComponent.DismemberedLimbs;
    apSpawnRequest->IsPlayer = characterComponent.IsPlayer();
    apSpawnRequest->IsWeaponDrawn = characterComponent.IsWeaponDrawn();
    apSpawnRequest->IsPlayerSummon = characterComponent.IsPlayerSummon();
    apSpawnRequest->IsDragon = characterComponent.IsDragon();
    apSpawnRequest->PlayerId = characterComponent.PlayerId;
    apSpawnRequest->SpawnGeneration = characterComponent.SpawnGeneration;

    const auto* pOwnerComponent = aRegistry.try_get<OwnerComponent>(aEntity);
    if (pOwnerComponent)
    {
        apSpawnRequest->OwnershipEpoch = pOwnerComponent->OwnershipEpoch;
        apSpawnRequest->FollowerOfPlayerId = ConnectedFollowerPlayer(aRegistry, *pOwnerComponent);
    }

    const auto* pFormIdComponent = aRegistry.try_get<FormIdComponent>(aEntity);
    if (pFormIdComponent)
    {
        apSpawnRequest->FormId = pFormIdComponent->Id;
    }

    const auto* pInventoryComponent = aRegistry.try_get<InventoryComponent>(aEntity);
    if (pInventoryComponent)
    {
        apSpawnRequest->InventoryContent = pInventoryComponent->Content;
    }

    const auto* pActorValuesComponent = aRegistry.try_get<ActorValuesComponent>(aEntity);
    if (pActorValuesComponent)
    {
        apSpawnRequest->InitialActorValues = pActorValuesComponent->CurrentActorValues;
    }

    if (characterComponent.BaseId)
    {
        apSpawnRequest->BaseId = characterComponent.BaseId.Id;
    }

    if (characterComponent.LeveledNpcPickId)
    {
        apSpawnRequest->LeveledNpcPickId = characterComponent.LeveledNpcPickId.Id;
    }

    const auto* pMovementComponent = aRegistry.try_get<MovementComponent>(aEntity);
    if (pMovementComponent)
    {
        apSpawnRequest->Position = pMovementComponent->Position;
        apSpawnRequest->Rotation.x = pMovementComponent->Rotation.x;
        apSpawnRequest->Rotation.y = pMovementComponent->Rotation.z;
    }

    const auto* pCellIdComponent = aRegistry.try_get<CellIdComponent>(aEntity);
    if (pCellIdComponent)
    {
        apSpawnRequest->CellId = pCellIdComponent->Cell;
    }

    auto& animationComponent = aRegistry.get<AnimationComponent>(aEntity);
    apSpawnRequest->ActionsToReplay = animationComponent.ActionsReplayCache.FormRefinedReplayChain();
}

// Every two seconds, an actor whose owner is out of range or silent, and does not report it loaded, goes to another player.
// The next owner is first a game that reports the actor loaded, then one whose range takes it in (PresenceService::NextOwners).
// Silent means no movement snapshot for a while, as in a paused game; a silent player is never a candidate.
// Two sweeps in a row are needed, so a player crossing a cell edge does not bounce actors back and forth.
// Players' characters, summons and dead actors stay where they are.
void CharacterService::HandOffAbandonedActors() const noexcept
{
    static std::chrono::steady_clock::time_point s_nextSweep;
    static TiltedPhoques::Map<uint32_t, uint32_t> s_outOfRangeSweeps; // entity -> consecutive sweeps out of range
    static TiltedPhoques::Map<uint32_t, uint32_t> s_nearerMemberSweeps; // entity -> consecutive sweeps with a member near it and the leader far

    const auto now = std::chrono::steady_clock::now();
    if (now < s_nextSweep)
        return;
    s_nextSweep = now + 2s;

    const auto isSilent = [now](const Player* apPlayer) { return PresenceService::IsSilent(apPlayer, now); };
    const PresenceService& presence = m_world.GetPresenceService();

    TiltedPhoques::Set<uint32_t> playerCharacters;
    for (auto pPlayer : m_world.GetPlayerManager())
        if (pPlayer->GetCharacter())
            playerCharacters.insert(World::ToInteger(*pPlayer->GetCharacter()));

    auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent>();
    for (auto entity : view)
    {
        const uint32_t cId = World::ToInteger(entity);
        auto& ownerComponent = view.get<OwnerComponent>(entity);
        auto& characterComponent = view.get<CharacterComponent>(entity);
        auto& cellIdComponent = view.get<CellIdComponent>(entity);

        Player* pOwner = ownerComponent.GetOwner();
        if (!pOwner || playerCharacters.count(cId) || characterComponent.IsPlayerSummon() || characterComponent.IsDead())
        {
            s_outOfRangeSweeps.erase(cId);
            s_nearerMemberSweeps.erase(cId);
            continue;
        }

        // A follower stays with her player's game while it is connected: another game's AI has no reason to follow, and the others show her as a copy.
        if (ConnectedFollowerPlayer(m_world, ownerComponent) == pOwner->GetId())
        {
            s_outOfRangeSweeps.erase(cId);
            s_nearerMemberSweeps.erase(cId);
            continue;
        }

        // An owner whose game still reports it running the actor keeps it, wherever the server puts that game's loaded cells.
        // One just given it keeps it a few seconds, until its game's reports can say so.
        constexpr auto cNewOwnerGrace = 6s;
        const bool ownerSilent = isSilent(pOwner);
        const bool cOwnerHasIt = presence.LoadedAs(entity, pOwner->GetId(), now) == ActorPresenceKind::Own || now - ownerComponent.OwnerSince < cNewOwnerGrace;
        if (!ownerSilent && (cOwnerHasIt || pOwner->GetCellComponent().IsInRange(cellIdComponent, characterComponent.IsDragon())))
        {
            s_outOfRangeSweeps.erase(cId);

            // The leader's game keeps an NPC far from its player only while no member is near it (OwnerSpread).
            // Not in a fight: an archer or a mage can fight the leader from beyond that, and is not moved to another game mid-fight.
            Player* pMember = now - ownerComponent.OwnerSince < cNewOwnerGrace || presence.FightOf(entity) != 0
                                  ? nullptr
                                  : FindMemberToTakeOver(entity, ownerComponent, cellIdComponent, characterComponent.IsDragon(), now);
            if (!pMember)
                s_nearerMemberSweeps.erase(cId);
            else if (++s_nearerMemberSweeps[cId] >= 2)
            {
                s_nearerMemberSweeps.erase(cId);
                if (CanClaimOwnership(pMember, entity, ownerComponent.OwnershipEpoch, OwnershipTransferReason::NearestMember))
                    TransferOwnership(pMember, entity, OwnershipTransferReason::NearestMember);
            }
            continue;
        }
        s_nearerMemberSweeps.erase(cId);

        const auto cCandidates = presence.NextOwners(entity, ownerComponent, cellIdComponent, characterComponent.IsDragon(), now);
        Player* pCandidate = cCandidates.empty() || isSilent(cCandidates.front()) ? nullptr : cCandidates.front();

        if (!pCandidate)
        {
            s_outOfRangeSweeps.erase(cId);
            continue;
        }

        if (++s_outOfRangeSweeps[cId] < 2)
            continue;
        s_outOfRangeSweeps.erase(cId);

        const auto& ownerCell = pOwner->GetCellComponent().CenterCoords;
        const auto cCandidateKind = presence.LoadedAs(entity, pCandidate->GetId(), now);
        const char* pCandidateHas = !cCandidateKind ? "who is in range"
                                    : *cCandidateKind == ActorPresenceKind::StandIn ? "whose game shows it on a stand-in"
                                                                                     : "whose game has it loaded";
        spdlog::info("Handoff: actor {:X} at grid ({}, {}) from player {:X} at ({}, {}), {}, to player {:X} {}", cId, cellIdComponent.CenterCoords.X,
                     cellIdComponent.CenterCoords.Y, pOwner->GetConnectionId(), ownerCell.X, ownerCell.Y,
                     ownerSilent ? "silent (paused or loading)" : "out of its range and not running it", pCandidate->GetConnectionId(), pCandidateHas);

        // TransferOwnership bumps the epoch, so a late update from the former owner arrives stale.
        ownerComponent.InvalidOwners.clear();
        TransferOwnership(pCandidate, entity, OwnershipTransferReason::OwnerUnavailable);
    }
}

Player* CharacterService::FindMemberToTakeOver(
    const entt::entity aEntity, const OwnerComponent& acOwner, const CellIdComponent& acCell, const bool aIsDragon, const std::chrono::steady_clock::time_point aNow) const noexcept
{
    Player* pLeader = acOwner.GetOwner();
    auto& partyService = m_world.GetPartyService();
    const auto* pMovement = m_world.try_get<MovementComponent>(aEntity);
    if (!pLeader || !pMovement || !partyService.IsPlayerLeader(pLeader))
        return nullptr;
    const PartyService::Party* pParty = partyService.GetPlayerParty(pLeader);
    if (!pParty)
        return nullptr;

    const NearestMember cNearest = FindNearestMember(m_world, *pParty, aEntity, acOwner, acCell, pMovement->Position, aIsDragon, aNow);
    if (!cNearest.pPlayer || !OwnerSpread::MemberTakesFromLeader(DistanceFrom(m_world, pLeader, acCell, pMovement->Position), cNearest.Distance))
        return nullptr;
    return cNearest.pPlayer;
}

void CharacterService::OnUpdate(const UpdateEvent&) const noexcept
{
    ProcessFactionsChanges();
    HandOffAbandonedActors();
    ProcessPendingLeaderAssignments();
    SweepHolds();
}

bool CharacterService::HoldUntilSettled(Player* apLeader, const entt::entity aEntity, const uint32_t aOwnershipEpoch, const OwnershipTransferReason aReason) const noexcept
{
    // Right after the leader's arrival (a load), its game is still bringing actors in.
    // Handed over at once, they go back and forth between the games, so the hand-over waits until the arrival is 10 s old.
    constexpr auto cGrace = std::chrono::seconds(10);
    const auto cReadyAt = apLeader->GetLastCellEnterAt() + cGrace;
    if (std::chrono::steady_clock::now() >= cReadyAt)
        return false;

    m_pendingLeaderAssignments.push_back(PendingLeaderAssignment{apLeader->GetId(), aEntity, aOwnershipEpoch, cReadyAt, aReason});
    spdlog::info("Ownership: {} of actor {:X} by player {:X} held back until 10 s after their arrival", GetOwnershipTransferReasonName(aReason), World::ToInteger(aEntity),
                 apLeader->GetId());
    return true;
}

void CharacterService::ProcessPendingLeaderAssignments() const noexcept
{
    if (m_pendingLeaderAssignments.empty())
        return;

    const auto cNow = std::chrono::steady_clock::now();
    for (auto it = m_pendingLeaderAssignments.begin(); it != m_pendingLeaderAssignments.end();)
    {
        if (cNow < it->ReadyAt)
        {
            ++it;
            continue;
        }

        const PendingLeaderAssignment cPending = *it;
        it = m_pendingLeaderAssignments.erase(it);

        Player* const pPlayer = m_world.GetPlayerManager().GetById(cPending.PlayerId);
        if (!pPlayer || !m_world.valid(cPending.Entity))
            continue;
        // The rules are checked again: another arrival, a hand-over or a departure may have come meanwhile.
        // What is held back here is never a grab, so an NPC that died in the wait stays with its owner.
        if (const auto* pCharacter = m_world.try_get<CharacterComponent>(cPending.Entity); pCharacter && pCharacter->IsDead())
            continue;
        if (CanClaimOwnership(pPlayer, cPending.Entity, cPending.OwnershipEpoch, cPending.Reason))
            TransferOwnership(pPlayer, cPending.Entity, cPending.Reason);
    }
}

void CharacterService::SendSpawn(World& aWorld, Player* apPlayer, const entt::entity aEntity, const CharacterSpawnRequest& acSpawn) noexcept
{
    apPlayer->Send(acSpawn);

    // A dead body lies as it came to rest at its owner's, not as this player's own ragdoll drops it.
    aWorld.GetCharacterService().SendRestPose(apPlayer, aEntity);
    aWorld.ctx().at<RagdollPoseService>().SendRestBodies(apPlayer, aEntity);

    // A cloak or a held spell started before this player saw the character.
    MagicStateService::SendTo(aWorld, apPlayer, aEntity);
    aWorld.ctx().at<FurnitureService>().SendPlayerSeat(apPlayer, aEntity);

    // The ride is said again every few seconds, so a player sent the pair later would see the rider walk in the saddle until then.
    // Both copies have to be there for it to take: it goes with the rider and with the horse, whichever comes second.
    const double cNow = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto cTell = [&](const entt::entity aRider, const MountSync::Ride& acRide)
    {
        const entt::entity cMount = static_cast<entt::entity>(acRide.Mount);
        const auto* pRiderOwner = aWorld.try_get<OwnerComponent>(aRider);
        const auto* pMountOwner = aWorld.valid(cMount) ? aWorld.try_get<OwnerComponent>(cMount) : nullptr;
        if (!pRiderOwner || !pMountOwner || pRiderOwner->GetOwner() == apPlayer ||
            !MountSync::TellLateLoader(acRide, pRiderOwner->OwnershipEpoch, pMountOwner->OwnershipEpoch, cNow))
            return;
        NotifyMount notify;
        notify.RiderId = World::ToInteger(aRider);
        notify.MountId = acRide.Mount;
        notify.RiderOwnershipEpoch = pRiderOwner->OwnershipEpoch;
        notify.MountOwnershipEpoch = pMountOwner->OwnershipEpoch;
        // Already riding: the copy is put in the saddle, with no climb.
        notify.IsRefresh = true;
        apPlayer->Send(notify);
    };
    if (const auto* pRide = aWorld.try_get<RideComponent>(aEntity))
        cTell(aEntity, pRide->Ride);
    const auto cRides = aWorld.view<RideComponent>();
    for (const auto cRider : cRides)
        if (cRider != aEntity && static_cast<entt::entity>(cRides.get<RideComponent>(cRider).Ride.Mount) == aEntity)
            cTell(cRider, cRides.get<RideComponent>(cRider).Ride);

    // An NPC sitting or sleeping in its furniture is said again every few seconds too: a player sent it later sees it sit at once.
    const auto* pSeat = aWorld.try_get<SeatComponent>(aEntity);
    const auto* pOwner = aWorld.try_get<OwnerComponent>(aEntity);
    if (pSeat && pOwner && pOwner->GetOwner() != apPlayer && pSeat->RelayedAt.time_since_epoch().count())
    {
        const auto cSince = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - pSeat->RelayedAt).count();
        if (cSince >= 0 && SeatCatchUp::Tell(pSeat->Relayed.State, pSeat->RelayedEpoch, pOwner->OwnershipEpoch, pSeat->Relayed.FurnitureBaseId != 0, static_cast<uint64_t>(cSince)))
        {
            NpcFurnitureWord word{};
            word.NpcId = World::ToInteger(aEntity);
            word.FurnitureModId = pSeat->Relayed.FurnitureModId;
            word.FurnitureBaseId = pSeat->Relayed.FurnitureBaseId;
            word.Marker = pSeat->Relayed.Marker;
            word.State = pSeat->Relayed.State;
            word.OwnershipEpoch = pSeat->RelayedEpoch;
            word.Placement = pSeat->Relayed.Placement;
            NotifyNpcFurniture notify;
            notify.Words.push_back(word);
            apPlayer->Send(notify);
        }
    }
}

void CharacterService::OnCharacterExteriorCellChange(const CharacterExteriorCellChangeEvent& acEvent) const noexcept
{
    // The character's own place is set now, before it is sent: its first actions there may come before its movement, and go by it.
    if (auto* pCell = m_world.try_get<CellIdComponent>(acEvent.Entity))
        *pCell = CellIdComponent{acEvent.CellId, acEvent.WorldSpaceId, acEvent.CurrentCoords};

    CharacterSpawnRequest spawnMessage;
    Serialize(m_world, acEvent.Entity, &spawnMessage);

    NotifyRemoveCharacter removeMessage;
    removeMessage.ServerId = World::ToInteger(acEvent.Entity);

    for (auto pPlayer : m_world.GetPlayerManager())
    {
        if (acEvent.Owner == pPlayer)
            continue;

        const auto& viewer = pPlayer->GetCellComponent();
        const bool sameWorld = viewer.WorldSpaceId == acEvent.WorldSpaceId;
        const bool inGrid = GridCellCoords::IsCellInGridCell(acEvent.CurrentCoords, viewer.CenterCoords, false);

        if (!sameWorld || !inGrid)
            pPlayer->Send(removeMessage);
        else
            SendSpawn(m_world, pPlayer, acEvent.Entity, spawnMessage);
    }
}

void CharacterService::OnCharacterInteriorCellChange(const CharacterInteriorCellChangeEvent& acEvent) const noexcept
{
    // The character's own place is set now, before it is sent: its first actions there may come before its movement, and go by it.
    if (auto* pCell = m_world.try_get<CellIdComponent>(acEvent.Entity))
        *pCell = CellIdComponent{acEvent.NewCell};

    CharacterSpawnRequest spawnMessage;
    Serialize(m_world, acEvent.Entity, &spawnMessage);

    NotifyRemoveCharacter removeMessage;
    removeMessage.ServerId = World::ToInteger(acEvent.Entity);

    for (auto pPlayer : m_world.GetPlayerManager())
    {
        if (acEvent.Owner == pPlayer)
            continue;

        if (acEvent.NewCell == pPlayer->GetCellComponent().Cell)
            SendSpawn(m_world, pPlayer, acEvent.Entity, spawnMessage);
        else
            pPlayer->Send(removeMessage);
    }
}

void CharacterService::OnAssignCharacterRequest(const PacketEvent<AssignCharacterRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;
    const auto& refId = message.ReferenceId;

    const auto isPlayer = (refId.ModId == 0 && refId.BaseId == 0x14);
    const auto isCustom = isPlayer || refId.ModId == std::numeric_limits<uint32_t>::max();

    // An actor the party's leader switched off stays off in every game.
    // Given a character, the copy made for another game would switch it back on there.
    const auto* pParty = isCustom ? nullptr : m_world.GetPartyService().GetPlayerParty(acMessage.pPlayer);
    if (pParty)
    {
        const auto cEnabled = pParty->ReferenceEnabled.Find(refId);
        if (cEnabled && !*cEnabled)
        {
            spdlog::info("ReferenceEnable: {:X}:{:X} asked for a character, but the leader switched it off; player {} is told to switch it off too", refId.ModId, refId.BaseId,
                         acMessage.pPlayer->GetId());

            NotifyReferenceEnable notify{};
            notify.Changes.push_back(ReferenceEnableState{refId, false});
            acMessage.pPlayer->Send(notify);

            // No epoch: the game ends its wait and keeps the actor out of the server.
            AssignCharacterResponse response{};
            response.Cookie = message.Cookie;
            acMessage.pPlayer->Send(response);
            return;
        }
    }

    // Check if id is the player
    if (!isCustom)
    {
        // Look for the character
        auto view = m_world.view<FormIdComponent, ActorValuesComponent, CharacterComponent, MovementComponent, CellIdComponent, OwnerComponent, InventoryComponent>();

        const auto itor = std::find_if(
            std::begin(view), std::end(view),
            [view, refId](auto entity)
            {
                const auto& formIdComponent = view.get<FormIdComponent>(entity);

                return formIdComponent.Id == refId;
            });

        if (itor != std::end(view))
        {
            spdlog::debug("FormId: {:x}:{:x} is already managed", refId.ModId, refId.BaseId);

            auto& ownerComponent = view.get<OwnerComponent>(*itor);
            auto& characterComponent = view.get<CharacterComponent>(*itor);
            // Told first: the answer says whose follower this is, and the player's own follower goes straight to them.
            const uint32_t cFollowerBefore = ownerComponent.FollowerOfPlayerId;
            if (message.IsFollower)
            {
                TakeFollower(acMessage.pPlayer, *itor);
                // The answer below tells this game whose she is, so a follower the rules will not give it is settled first.
                if (ConnectedFollowerPlayer(m_world, ownerComponent) == acMessage.pPlayer->GetId() && ownerComponent.GetOwner() != acMessage.pPlayer &&
                    !CanClaimOwnership(acMessage.pPlayer, *itor, ownerComponent.OwnershipEpoch, OwnershipTransferReason::LeaderClaim, true))
                    ownerComponent.FollowerOfPlayerId = cFollowerBefore;
            }
            // Her player's game no longer has her as a follower: an older save, or she was dismissed while she was not loaded.
            if (!message.IsFollower && ownerComponent.FollowerOfPlayerId == acMessage.pPlayer->GetId())
            {
                ownerComponent.FollowerOfPlayerId = 0;
                spdlog::info("Follower: actor {:X} is no longer player {:X}'s", World::ToInteger(*itor), acMessage.pPlayer->GetId());
            }
            const bool cOwnFollower = ConnectedFollowerPlayer(m_world, ownerComponent) == acMessage.pPlayer->GetId();

            const bool isOwner = ownerComponent.GetOwner() == acMessage.pPlayer;
            const auto& cPartyService = m_world.GetPartyService();
            const bool cWorthAsking =
                LeaderAssignmentRules::WorthAsking(cPartyService.IsPlayerInParty(acMessage.pPlayer), cPartyService.IsPlayerLeader(acMessage.pPlayer), characterComponent.IsDead());
            const bool transferToLeader =
                !isOwner && !cOwnFollower && cWorthAsking && CanClaimOwnership(acMessage.pPlayer, *itor, ownerComponent.OwnershipEpoch, OwnershipTransferReason::LeaderAssignment);

            if (!characterComponent.LeveledNpcPickId && message.LeveledNpcPickId != GameId{})
            {
                characterComponent.LeveledNpcPickId = FormIdComponent(message.LeveledNpcPickId);
                spdlog::debug(
                    "Stored previously unknown leveled NPC pick {:x}:{:x} for FormId {:x}:{:x}",
                    message.LeveledNpcPickId.ModId,
                    message.LeveledNpcPickId.BaseId,
                    refId.ModId,
                    refId.BaseId);
            }

            AssignCharacterResponse response{};
            response.Cookie = message.Cookie;
            response.Owner = isOwner;
            PopulateAssignmentResponse(*itor, response);
            // Empty for players and for actors whose owner sent none; see the client's base form check.
            response.BaseId = characterComponent.BaseId.Id;
            acMessage.pPlayer->Send(response);

            // The assignment response establishes a remote component before the grant arrives.
            if (transferToLeader && !HoldUntilSettled(acMessage.pPlayer, *itor, ownerComponent.OwnershipEpoch, OwnershipTransferReason::LeaderAssignment))
                TransferOwnership(acMessage.pPlayer, *itor, OwnershipTransferReason::LeaderAssignment);

            const bool cMoved = cOwnFollower && GiveFollowerToPlayer(acMessage.pPlayer, *itor, cFollowerBefore);
            if (cMoved || ownerComponent.FollowerOfPlayerId != cFollowerBefore)
                ResendToViewers(*itor);

            return;
        }
    }

    // Every game runs the same scripts: another game may have spawned this one already.
    if (!isPlayer && isCustom && PairScriptSpawn(acMessage))
        return;

    // This entity has no owner create it
    CreateCharacter(acMessage);
}

bool CharacterService::PairScriptSpawn(const PacketEvent<AssignCharacterRequest>& acMessage) const noexcept
{
    const auto& message = acMessage.Packet;
    if (!bShareScriptSpawns || message.IsPlayerSummon || message.FormId == GameId{})
        return false;

    auto& partyService = m_world.GetPartyService();
    const auto* pParty = partyService.GetPlayerParty(acMessage.pPlayer);
    if (!pParty)
        return false;

    TwinSpawn::Spawn spawn{};
    spawn.PlayerId = acMessage.pPlayer->GetId();
    spawn.BaseId = message.FormId;
    spawn.LeveledNpcPickId = message.LeveledNpcPickId;
    spawn.CellId = message.CellId;
    spawn.WorldSpaceId = message.WorldSpaceId;
    spawn.Position = message.Position;

    const auto now = std::chrono::steady_clock::now();
    Vector<entt::entity> entities;
    std::vector<TwinSpawn::Candidate> candidates;
    auto view = m_world.view<ScriptSpawnComponent, CharacterComponent, OwnerComponent>();
    for (const auto cEntity : view)
    {
        Player* pOwner = view.get<OwnerComponent>(cEntity).GetOwner();
        if (!pOwner)
            continue;

        const auto& cSpawned = view.get<ScriptSpawnComponent>(cEntity);
        const auto& cCharacter = view.get<CharacterComponent>(cEntity);

        auto& candidate = candidates.emplace_back();
        candidate.OwnerId = pOwner->GetId();
        candidate.SameParty = partyService.GetPlayerParty(pOwner) == pParty;
        candidate.BaseId = cCharacter.BaseId.Id;
        candidate.LeveledNpcPickId = cCharacter.LeveledNpcPickId.Id;
        candidate.CellId = cSpawned.CellId;
        candidate.WorldSpaceId = cSpawned.WorldSpaceId;
        candidate.SpawnPosition = cSpawned.SpawnPosition;
        candidate.AgeSeconds = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(now - cSpawned.SpawnedAt).count());
        candidate.PairedWithPlayer = std::find(cSpawned.PairedPlayers.begin(), cSpawned.PairedPlayers.end(), spawn.PlayerId) != cSpawned.PairedPlayers.end();
        entities.push_back(cEntity);
    }

    const auto cTwin = TwinSpawn::FindTwin(spawn, candidates);
    if (!cTwin)
    {
        // The same kind from another game of the party, here lately, missed for a reason the log shows.
        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            const TwinSpawn::Miss cMiss = TwinSpawn::Check(spawn, candidates[i]);
            if (TwinSpawn::IsNearMiss(cMiss))
                spdlog::debug(
                    "ScriptSpawn: player {:X}'s spawn of {:X}:{:X} in cell {:X} is not paired with actor {:X} (player {:X}'s, {:.0f} units, {} s): {}", spawn.PlayerId,
                    spawn.BaseId.ModId, spawn.BaseId.BaseId, spawn.CellId.BaseId, World::ToInteger(entities[i]), candidates[i].OwnerId,
                    glm::distance(candidates[i].SpawnPosition, spawn.Position), candidates[i].AgeSeconds, TwinSpawn::Describe(cMiss));
        }
        return false;
    }

    const entt::entity cEntity = entities[*cTwin];
    auto& spawned = view.get<ScriptSpawnComponent>(cEntity);
    spawned.PairedPlayers.push_back(spawn.PlayerId);

    AssignCharacterResponse response{};
    response.Cookie = message.Cookie;
    response.Owner = false;
    PopulateAssignmentResponse(cEntity, response);
    response.BaseId = view.get<CharacterComponent>(cEntity).BaseId.Id;
    acMessage.pPlayer->Send(response);

    spdlog::debug(
        "ScriptSpawn: player {:X}'s spawn of {:X}:{:X} in cell {:X} stands for actor {:X}, which player {:X}'s game made {} s before, {:.0f} units away ({} game(s) paired)",
        spawn.PlayerId, spawn.BaseId.ModId, spawn.BaseId.BaseId, spawn.CellId.BaseId, World::ToInteger(cEntity), candidates[*cTwin].OwnerId, candidates[*cTwin].AgeSeconds,
        glm::distance(candidates[*cTwin].SpawnPosition, spawn.Position), spawned.PairedPlayers.size());
    return true;
}

bool CharacterService::HasPairedGame(const entt::entity aEntity) const noexcept
{
    const auto* pSpawned = m_world.try_get<ScriptSpawnComponent>(aEntity);
    if (!pSpawned)
        return false;

    const auto& playerManager = m_world.GetPlayerManager();
    return std::any_of(
        pSpawned->PairedPlayers.begin(), pSpawned->PairedPlayers.end(), [&playerManager](uint32_t aPlayerId) { return playerManager.GetById(aPlayerId) != nullptr; });
}

void CharacterService::OnOwnershipTransferRequest(const PacketEvent<RequestOwnershipTransfer>& acMessage) const noexcept
{
    const auto& message = acMessage.Packet;

    const entt::entity cEntity = static_cast<entt::entity>(message.ServerId);
    const auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent, MovementComponent>();
    const auto it = view.find(cEntity);
    if (it == view.end())
    {
        spdlog::debug("Ignored ownership release from player {:X} for missing actor {:X}", acMessage.pPlayer->GetId(), message.ServerId);
        return;
    }

    auto& ownerComponent = view.get<OwnerComponent>(*it);
    if (ownerComponent.GetOwner() != acMessage.pPlayer || ownerComponent.OwnershipEpoch != message.OwnershipEpoch)
    {
        const uint32_t ownerId = ownerComponent.GetOwner() ? ownerComponent.GetOwner()->GetId() : 0;
        spdlog::debug(
            "Ignored ownership release from player {:X} for actor {:X}; current owner is {:X} and requested epoch {} does not match {}",
            acMessage.pPlayer->GetId(), message.ServerId, ownerId, message.OwnershipEpoch, ownerComponent.OwnershipEpoch);
        return;
    }

    // The holder's game let go of the NPC it ran for a grab: it goes back, or stays by the usual rules.
    if (message.Reason == OwnershipReleaseReason::HoldEnded)
    {
        EndHold(cEntity, acMessage.pPlayer);
        return;
    }

    if (message.Reason != OwnershipReleaseReason::Relinquish && message.Reason != OwnershipReleaseReason::DeclineGrant &&
        message.Reason != OwnershipReleaseReason::Deleted)
    {
        spdlog::warn("Ignored ownership release with invalid reason from player {:X} for actor {:X}", acMessage.pPlayer->GetId(), message.ServerId);
        return;
    }

    // The release is newer than that game's last report: it no longer has the character, or cannot run it.
    m_world.GetPresenceService().ForgetReport(cEntity, acMessage.pPlayer->GetId());

    auto& characterComponent = view.get<CharacterComponent>(*it);
    if (characterComponent.IsPlayerSummon())
    {
        spdlog::info("Removing summon {:X} after player {:X} relinquished ownership", message.ServerId, acMessage.pPlayer->GetId());
        m_world.GetDispatcher().trigger(CharacterRemoveEvent(message.ServerId));
        return;
    }

    // A temporary actor its owner's game deleted leaves every game, as no other game could take it on.
    // A placed one is only let go, since every game has its own reference.
    // So is a spawn another game stands an actor of its own for (PairScriptSpawn): that game can take it on.
    if (message.Reason == OwnershipReleaseReason::Deleted && !m_world.all_of<FormIdComponent>(cEntity) && !(bShareScriptSpawns && HasPairedGame(cEntity)))
    {
        spdlog::info("Removing actor {:X}: player {:X}'s game, which ran it, deleted it", message.ServerId, acMessage.pPlayer->GetId());
        m_world.GetDispatcher().trigger(CharacterRemoveEvent(message.ServerId));
        return;
    }

    // A placed actor deleted in its owner's game is let go as any other.
    const bool cLetGo = message.Reason != OwnershipReleaseReason::DeclineGrant;
    if (cLetGo && (message.WorldSpaceId || message.CellId))
    {
        const auto* pFormIdComponent = m_world.try_get<FormIdComponent>(cEntity);
        if (pFormIdComponent && !IsLivingFollower(m_world, cEntity))
        {
            NotifyActorTeleport notify{};
            notify.FormId = pFormIdComponent->Id;
            notify.WorldSpaceId = message.WorldSpaceId;
            notify.CellId = message.CellId;
            notify.Position = message.Position;

            GameServer::Get()->SendToPlayers(notify, acMessage.pPlayer);
        }

        auto& cellIdComponent = view.get<CellIdComponent>(*it);
        cellIdComponent.WorldSpaceId = message.WorldSpaceId;
        cellIdComponent.Cell = message.CellId;
        cellIdComponent.CenterCoords = GridCellCoords::CalculateGridCellCoords(message.Position);

        auto& movementComponent = view.get<MovementComponent>(*it);
        movementComponent.Position = message.Position;
    }

    // A normal release starts a fresh search; a declined grant keeps the failed candidates so ownership cannot bounce.
    if (cLetGo)
        ownerComponent.InvalidOwners.clear();

    ownerComponent.InvalidOwners.push_back(acMessage.pPlayer);

    TransferToNextOwner(cEntity, OwnershipTransferReason::Relinquish);
}

void CharacterService::OnOwnershipTransferEvent(const OwnershipTransferEvent& acEvent) const noexcept
{
    // A disconnect starts a fresh search; previously unavailable clients may be ready now.
    const auto view = m_world.view<OwnerComponent>();
    if (const auto it = view.find(acEvent.Entity); it != view.end())
        view.get<OwnerComponent>(*it).InvalidOwners.clear();

    TransferToNextOwner(acEvent.Entity, OwnershipTransferReason::OwnerUnavailable);
}

void CharacterService::OnCharacterRemoveEvent(const CharacterRemoveEvent& acEvent) const noexcept
{
    m_restPoses.Forget(acEvent.ServerId);

    const auto view = m_world.view<OwnerComponent>();
    const auto it = view.find(static_cast<entt::entity>(acEvent.ServerId));
    if (it == view.end())
        return;

    GameServer::Get()->GetWorld().GetScriptService().HandleCharacterDestoy(*it);

    NotifyRemoveCharacter response;
    response.ServerId = acEvent.ServerId;
    response.Gone = true;

    for (auto pPlayer : m_world.GetPlayerManager())
        pPlayer->Send(response);

    m_world.destroy(*it);
    spdlog::debug("Character destroyed {:X}", acEvent.ServerId);
}

void CharacterService::SendRestPose(Player* apPlayer, const entt::entity aEntity) const noexcept
{
    const uint32_t cId = World::ToInteger(aEntity);
    const VRPose* pPose = m_restPoses.Resting(cId);
    if (!pPose)
        return;
    const auto* pCharacter = m_world.try_get<CharacterComponent>(aEntity);
    const auto* pOwner = m_world.try_get<OwnerComponent>(aEntity);
    const auto* pMovement = m_world.try_get<MovementComponent>(aEntity);
    const auto* pCell = m_world.try_get<CellIdComponent>(aEntity);
    if (!pCharacter || !pCharacter->IsDead() || !pOwner || pOwner->GetOwner() == apPlayer || !pMovement || !pCell)
        return;

    ReferenceUpdate update;
    Movement& movement = update.UpdatedMovement;
    movement.CellId = pCell->Cell;
    movement.WorldSpaceId = pCell->WorldSpaceId;
    movement.Position = pMovement->Position;
    movement.Rotation.x = pMovement->Rotation.x;
    movement.Rotation.y = pMovement->Rotation.z;
    movement.Variables = pMovement->Variables;
    movement.Direction = pMovement->Direction;
    movement.Flying = pMovement->Flying;
    update.UpdatedVRPose = *pPose;

    // Stamped with the owner's newest tick, as the relay stamps its reports: a copy made just now takes it as its first point, and
    // the owner's next reports come after it. A tick from another clock would make the copy drop them as older.
    ServerReferencesMoveRequest message;
    message.Tick = pMovement->Tick;
    message.Updates[cId] = update;
    // Reliable, unlike the stream: it is sent once.
    apPlayer->Send(message);
    spdlog::debug("Sent player {:X} the pose dead body {:X} rests in", apPlayer->GetId(), cId);
}

void CharacterService::ForgetRestPose(const entt::entity aEntity) const noexcept
{
    m_restPoses.Forget(World::ToInteger(aEntity));
}

bool CharacterService::ClaimToHold(Player* apPlayer, const entt::entity aEntity, const uint32_t aExpectedOwnershipEpoch) const noexcept
{
    const auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent>();
    const auto it = view.find(aEntity);
    if (it == view.end())
        return false;
    const auto& owner = view.get<OwnerComponent>(*it);
    const auto& character = view.get<CharacterComponent>(*it);
    auto& parties = m_world.GetPartyService();
    const auto* pParty = parties.GetPlayerParty(apPlayer);
    Player* const pOwner = owner.GetOwner();
    const uint32_t cOwnerId = pOwner ? pOwner->GetId() : 0;
    const uint32_t cId = World::ToInteger(aEntity);

    const auto hold = m_holds.find(cId);
    const bool cHeld = hold != m_holds.end() && GrabHold::IsLive(hold->second, cOwnerId, owner.OwnershipEpoch);
    const uint32_t cFollowerOf = ConnectedFollowerPlayer(m_world, owner);
    const auto* pMovement = m_world.try_get<MovementComponent>(aEntity);
    const double cNow = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto rest = m_holdRests.find(cId);

    GrabHold::ClaimToHold claim{};
    claim.SwitchOn = bGrabHolds;
    claim.Alive = !character.IsDead();
    claim.IsPlayer = character.IsPlayer();
    claim.IsMount = character.IsMount();
    claim.Temporary = !m_world.all_of<FormIdComponent>(aEntity);
    claim.HasOwner = pOwner != nullptr;
    claim.OwnedBySender = pOwner == apPlayer;
    claim.OwnersInSameParty = pParty && pOwner && pParty == parties.GetPlayerParty(pOwner);
    claim.InRange = apPlayer->GetCellComponent() && apPlayer->GetCellComponent().IsInRange(view.get<CellIdComponent>(*it), character.IsDragon());
    claim.EpochMatches = aExpectedOwnershipEpoch != 0 && aExpectedOwnershipEpoch == owner.OwnershipEpoch;
    claim.OthersFollower = cFollowerOf && cFollowerOf != apPlayer->GetId();
    claim.FlyingDragon = character.IsDragon() && pMovement && pMovement->Flying;
    claim.HeldByOther = cHeld && hold->second.Holder != apPlayer->GetId();
    claim.Resting = rest != m_holdRests.end() && GrabHold::IsResting(rest->second, apPlayer->GetId(), cNow);
    if (!GrabHold::MayClaimToHold(claim))
    {
        spdlog::debug(
            "Rejected a claim to hold actor {:X} from player {:X} (alive {}, owner {:X}, same party {}, in range {}, epoch {} of {}, follower of {:X}, held by other {})", cId,
            apPlayer->GetId(), claim.Alive, cOwnerId, claim.OwnersInSameParty, claim.InRange, aExpectedOwnershipEpoch, owner.OwnershipEpoch, cFollowerOf, claim.HeldByOther);
        return false;
    }

    if (!TransferOwnership(apPlayer, aEntity, OwnershipTransferReason::Hold))
        return false;
    m_holds[cId] = GrabHold::Hold{apPlayer->GetId(), cOwnerId, owner.OwnershipEpoch, parties.IsPlayerLeader(apPlayer), cNow};
    return true;
}

void CharacterService::EndHold(const entt::entity aEntity, const Player* apHolder) const noexcept
{
    const uint32_t cId = World::ToInteger(aEntity);
    const auto hold = m_holds.find(cId);
    if (hold == m_holds.end())
        return;
    const GrabHold::Hold cHold = hold->second;
    if (apHolder && apHolder->GetId() != cHold.Holder)
        return;
    m_holds.erase(hold);

    const auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent>();
    const auto it = view.find(aEntity);
    if (it == view.end())
        return;
    const auto& owner = view.get<OwnerComponent>(*it);
    const auto& character = view.get<CharacterComponent>(*it);
    Player* const pOwner = owner.GetOwner();
    const bool cLive = GrabHold::IsLive(cHold, pOwner ? pOwner->GetId() : 0, owner.OwnershipEpoch);

    auto& parties = m_world.GetPartyService();
    Player* const pFormer = m_world.GetPlayerManager().GetById(cHold.FormerOwner);
    const auto* pParty = pOwner ? parties.GetPlayerParty(pOwner) : nullptr;
    const bool cFormerFits = pFormer && pParty && pParty == parties.GetPlayerParty(pFormer) && !PresenceService::IsSilent(pFormer, std::chrono::steady_clock::now()) &&
                             pFormer->GetCellComponent() && pFormer->GetCellComponent().IsInRange(view.get<CellIdComponent>(*it), character.IsDragon());

    switch (GrabHold::OnEnd(cHold, cLive, !apHolder || apHolder->GetId() == cHold.Holder, character.IsDead(), cFormerFits))
    {
    case GrabHold::End::Return:
        spdlog::debug("Hold: actor {:X} goes back to player {:X}: {}", cId, cHold.FormerOwner, apHolder ? "its holder let go" : "the hold ran too long");
        TransferOwnership(pFormer, aEntity, OwnershipTransferReason::HoldEnded);
        if (!apHolder)
            m_holdRests[cId] = GrabHold::RestAfterExpiry(cHold, std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
        break;
    case GrabHold::End::Keep:
        spdlog::debug(
            "Hold: actor {:X} stays with player {:X} (leader {}, dead {}, player {:X} can take it back {})", cId, cHold.Holder, cHold.Keep, character.IsDead(), cHold.FormerOwner,
            cFormerFits);
        break;
    case GrabHold::End::Ignore: break;
    }
}

void CharacterService::SweepHolds() const noexcept
{
    const double cNow = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    for (auto it = m_holdRests.begin(); it != m_holdRests.end();)
        it = cNow >= it->second.Until ? m_holdRests.erase(it) : std::next(it);
    if (m_holds.empty())
        return;

    Vector<uint32_t> expired;
    for (auto it = m_holds.begin(); it != m_holds.end();)
    {
        const entt::entity cEntity = static_cast<entt::entity>(it->first);
        const auto* pOwner = m_world.valid(cEntity) ? m_world.try_get<OwnerComponent>(cEntity) : nullptr;
        const bool cLive = pOwner && GrabHold::IsLive(it->second, pOwner->GetOwner() ? pOwner->GetOwner()->GetId() : 0, pOwner->OwnershipEpoch);
        if (!cLive)
        {
            it = m_holds.erase(it);
            continue;
        }
        if (GrabHold::IsExpired(it->second, cNow))
            expired.push_back(it->first);
        ++it;
    }
    for (const uint32_t cId : expired)
        EndHold(static_cast<entt::entity>(cId), nullptr);
}

bool CharacterService::IsHeld(uint32_t aActor) const noexcept
{
    const auto* pOwner = m_world.try_get<OwnerComponent>(static_cast<entt::entity>(aActor));
    const auto it = m_holds.find(aActor);
    return pOwner && pOwner->GetOwner() && it != m_holds.end() && GrabHold::IsLive(it->second, pOwner->GetOwner()->GetId(), pOwner->OwnershipEpoch);
}

void CharacterService::OnOwnershipClaimRequest(const PacketEvent<RequestOwnershipClaim>& acMessage) const noexcept
{
    const auto& message = acMessage.Packet;
    const entt::entity cEntity = static_cast<entt::entity>(message.ServerId);

    if ((message.ForRiding || message.ForHolding) && m_world.ctx().at<ExtensionService>().Reserved(message.ServerId))
        return;

    // A witness of the claimant's crime, asked for for it, goes at once, party leader or not: the offender's game runs the arrest.
    // Such a claim is answered by this rule only, before any other, whatever else it says; an ordinary claim never uses it.
    if (CrimeReports::ByWitnessRuleOnly(message))
    {
        const auto* pCharacter = m_world.try_get<CharacterComponent>(cEntity);
        const CrimeReports::WitnessClaim cClaim = m_crimeWitnesses.ClaimOf(
            bCrimeGuardHandoff, true, message.ServerId, acMessage.pPlayer->GetId(), message.ExpectedOwnershipEpoch, pCharacter && !pCharacter->IsDead(),
            std::chrono::steady_clock::now());
        if (CrimeReports::ServerGivesWitness(cClaim) && CanClaimOwnership(acMessage.pPlayer, cEntity, message.ExpectedOwnershipEpoch, OwnershipTransferReason::CrimeWitness))
        {
            m_crimeWitnesses.Take(message.ServerId, acMessage.pPlayer->GetId());
            TransferOwnership(acMessage.pPlayer, cEntity, OwnershipTransferReason::CrimeWitness);
        }
        else
            spdlog::debug(
                "Rejected a claim for crime witness {:X} from player {:X} (on {}, named {}, epoch {} as named {})", message.ServerId, acMessage.pPlayer->GetId(), cClaim.On,
                cClaim.NamedBySender, message.ExpectedOwnershipEpoch, cClaim.EpochAsNamed);
        return;
    }

    // A party member's horse, to ride: it goes at once, to the rider's game, if nobody sits on it.
    if (message.ForRiding)
    {
        const auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent>();
        const auto it = view.find(cEntity);
        if (it == view.end())
            return;
        const auto& owner = view.get<OwnerComponent>(*it);
        const auto& character = view.get<CharacterComponent>(*it);
        auto& parties = m_world.GetPartyService();
        const auto* pParty = parties.GetPlayerParty(acMessage.pPlayer);
        Player* pOwner = owner.GetOwner();

        MountSync::ClaimToRide claim{};
        claim.SharingOn = bPartyHorses;
        claim.IsMount = character.IsMount();
        claim.Dead = character.IsDead();
        claim.IsPlayer = character.IsPlayer();
        claim.HasOwner = pOwner != nullptr;
        claim.OwnedBySender = pOwner == acMessage.pPlayer;
        claim.OwnersInSameParty = pParty && pOwner && pParty == parties.GetPlayerParty(pOwner);
        claim.InRange = acMessage.pPlayer->GetCellComponent() && acMessage.pPlayer->GetCellComponent().IsInRange(view.get<CellIdComponent>(*it), character.IsDragon());
        claim.EpochMatches = message.ExpectedOwnershipEpoch != 0 && message.ExpectedOwnershipEpoch == owner.OwnershipEpoch;
        // A ride said lately, by a rider whose game is not the claimant's.
        const double cNow = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const auto cRides = m_world.view<RideComponent, OwnerComponent>();
        for (const auto cRider : cRides)
        {
            const auto& ride = cRides.get<RideComponent>(cRider).Ride;
            if (static_cast<entt::entity>(ride.Mount) == cEntity && cRides.get<OwnerComponent>(cRider).GetOwner() != acMessage.pPlayer &&
                cNow - ride.ToldAt < MountSync::kSeatStaleAfter)
                claim.RiddenByOther = true;
        }
        if (!MountSync::MayClaimToRide(claim))
        {
            spdlog::debug(
                "Rejected a claim to ride horse {:X} from player {:X} (sharing {}, mount {}, dead {}, owner {:X}, same party {}, in range {}, epoch {} of {}, ridden {})",
                message.ServerId, acMessage.pPlayer->GetId(), claim.SharingOn, claim.IsMount, claim.Dead, pOwner ? pOwner->GetId() : 0, claim.OwnersInSameParty, claim.InRange,
                message.ExpectedOwnershipEpoch, owner.OwnershipEpoch, claim.RiddenByOther);
            return;
        }
        TransferOwnership(acMessage.pPlayer, cEntity, OwnershipTransferReason::Mount);
        return;
    }

    // A living NPC a party member grabbed in its ragdoll goes at once, to the grabber's game, for the hold.
    // A body goes by the rules below, and so does every claim with Gameplay:bGrabHolds off, as before.
    if (message.ForHolding && bGrabHolds)
    {
        const auto* pCharacter = m_world.try_get<CharacterComponent>(cEntity);
        // Refused, the party leader's claim still goes by the leader's rules, as before.
        if (pCharacter && !pCharacter->IsDead() &&
            (ClaimToHold(acMessage.pPlayer, cEntity, message.ExpectedOwnershipEpoch) || !m_world.GetPartyService().IsPlayerLeader(acMessage.pPlayer)))
            return;
    }

    // A killing blow's claim goes at once: the killer's game is waiting to kill it.
    if (message.KillingBlow)
    {
        if (CanClaimOwnership(acMessage.pPlayer, cEntity, message.ExpectedOwnershipEpoch, OwnershipTransferReason::KillingBlow))
            TransferOwnership(acMessage.pPlayer, cEntity, OwnershipTransferReason::KillingBlow);
        return;
    }

    // A player's own follower goes by the follower rules, whatever the claim's epoch.
    if (message.Follower)
    {
        if (!m_world.view<OwnerComponent, CharacterComponent, CellIdComponent, FormIdComponent>().contains(cEntity))
            return;
        MakeFollowerOf(acMessage.pPlayer, cEntity, true);
        return;
    }

    if (!CanClaimOwnership(acMessage.pPlayer, cEntity, message.ExpectedOwnershipEpoch, OwnershipTransferReason::LeaderClaim))
        return;

    // The leader's game claims each NPC it loads, so after its arrival the claim waits like an assignment.
    // A dead body to drag goes at once.
    const auto* pCharacter = m_world.try_get<CharacterComponent>(cEntity);
    const bool cLivingNpc = pCharacter && !pCharacter->IsDead();
    if (cLivingNpc && HoldUntilSettled(acMessage.pPlayer, cEntity, message.ExpectedOwnershipEpoch, OwnershipTransferReason::LeaderClaim))
        return;
    TransferOwnership(acMessage.pPlayer, cEntity, OwnershipTransferReason::LeaderClaim);
}

void CharacterService::OnFollowerStateRequest(const PacketEvent<RequestFollowerState>& acMessage) const noexcept
{
    const auto& message = acMessage.Packet;
    const entt::entity cEntity = static_cast<entt::entity>(message.ServerId);

    // Only a reference of the game's own can be a follower: a temporary actor is not the same one in another game.
    const auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent, FormIdComponent>();
    if (!view.contains(cEntity))
    {
        spdlog::debug("Ignored follower state from player {:X} for missing actor {:X}", acMessage.pPlayer->GetId(), message.ServerId);
        return;
    }

    if (message.IsFollower)
    {
        MakeFollowerOf(acMessage.pPlayer, cEntity);
        return;
    }

    auto& ownerComponent = view.get<OwnerComponent>(cEntity);
    if (ownerComponent.FollowerOfPlayerId != acMessage.pPlayer->GetId())
        return;

    ownerComponent.FollowerOfPlayerId = 0;
    spdlog::info("Follower: actor {:X} is no longer player {:X}'s", message.ServerId, acMessage.pPlayer->GetId());
    ResendToViewers(cEntity);
}

void CharacterService::OnFollowerCommandRequest(const PacketEvent<RequestFollowerCommand>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;
    const entt::entity cEntity = static_cast<entt::entity>(message.ServerId);
    const uint32_t cSender = acMessage.pPlayer->GetId();

    const auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent>();
    if (!view.contains(cEntity))
        return;
    const auto& ownerComponent = view.get<OwnerComponent>(cEntity);
    const uint32_t cFollowerOf = ConnectedFollowerPlayer(m_world, ownerComponent);
    Player* pFollowerOf = cFollowerOf ? m_world.GetPlayerManager().GetById(cFollowerOf) : nullptr;

    // Her player's game answering the player who asked.
    if (message.Reply != FollowerCommand::kAsked)
    {
        Player* pAsker = m_world.GetPlayerManager().GetById(message.ReplyToPlayerId);
        auto& replyParties = m_world.GetPartyService();
        const auto* pSenderParty = replyParties.GetPlayerParty(acMessage.pPlayer);
        if (!FollowerCommand::IsReply(message.Reply) || !pAsker || cSender != cFollowerOf || !pSenderParty || pSenderParty != replyParties.GetPlayerParty(pAsker))
            return;
        NotifyFollowerCommand answer{};
        answer.ServerId = message.ServerId;
        answer.Command = message.Command;
        answer.Reply = message.Reply;
        answer.FromPlayerId = cSender;
        pAsker->Send(answer);
        return;
    }

    if (!FollowerCommand::IsCommand(message.Command) || !m_followerCommandRates[cSender].Allow(std::chrono::steady_clock::now()))
        return;

    auto& partyService = m_world.GetPartyService();
    const auto* pParty = partyService.GetPlayerParty(acMessage.pPlayer);
    const bool cSameParty = pParty && pFollowerOf && pParty == partyService.GetPlayerParty(pFollowerOf);
    const bool cInRange = acMessage.pPlayer->GetCellComponent().IsInRange(view.get<CellIdComponent>(cEntity), false);
    if (!FollowerCommand::MayAsk(cFollowerOf, pFollowerOf != nullptr, cSender, cSameParty) || !cInRange)
    {
        spdlog::debug(
            "Follower: player {:X}'s word to actor {:X} not passed on (her player {:X}, same party {}, in range {})", cSender, message.ServerId, cFollowerOf, cSameParty, cInRange);
        // The asker hears why nothing happens, rather than nothing.
        NotifyFollowerCommand refused{};
        refused.ServerId = message.ServerId;
        refused.Command = message.Command;
        refused.Reply = FollowerCommand::kOutOfReach;
        refused.FromPlayerId = cFollowerOf;
        acMessage.pPlayer->Send(refused);
        return;
    }

    NotifyFollowerCommand notify{};
    notify.ServerId = message.ServerId;
    notify.Command = message.Command;
    notify.FromPlayerId = cSender;
    pFollowerOf->Send(notify);
    spdlog::debug(
        "Follower: player {:X} told actor {:X} to {}; passed on to her player {:X}", cSender, message.ServerId, message.Command == FollowerCommand::kWait ? "wait" : "follow",
        cFollowerOf);
}

void CharacterService::OnStuckArrowRequest(const PacketEvent<RequestStuckArrow>& acMessage) const noexcept
{
    const auto& message = acMessage.Packet;
    const entt::entity cEntity = static_cast<entt::entity>(message.ServerId);

    // Only the game that runs the actor says what stuck in it.
    const auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent>();
    if (!view.contains(cEntity) || view.get<OwnerComponent>(cEntity).GetOwner() != acMessage.pPlayer)
        return;

    NotifyStuckArrow notify{};
    notify.ServerId = message.ServerId;
    notify.Ammo = message.Ammo;
    notify.Bone = message.Bone;
    notify.Position = message.Position;
    notify.Rotation = message.Rotation;
    notify.Scale = message.Scale;
    notify.Sequence = message.Sequence;
    notify.Removed = message.Removed;
    // Kept nowhere: the game saves no stuck arrows either.
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void CharacterService::OnDismemberRequest(const PacketEvent<RequestDismember>& acMessage) const noexcept
{
    const auto& message = acMessage.Packet;
    const entt::entity cEntity = static_cast<entt::entity>(message.ServerId);

    // Only the game that runs the actor says what was cut off it.
    const auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent>();
    if (!view.contains(cEntity) || view.get<OwnerComponent>(cEntity).GetOwner() != acMessage.pPlayer)
        return;

    // Kept with the actor, so a game that gets it later has the limbs cut off too.
    auto& characterComponent = view.get<CharacterComponent>(cEntity);
    characterComponent.DismemberedLimbs = LimbMask::Merge(characterComponent.DismemberedLimbs, message.Limbs);

    NotifyDismember notify{};
    notify.ServerId = message.ServerId;
    notify.Limbs = message.Limbs;
    notify.LastHitLimb = message.LastHitLimb;
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void CharacterService::OnPlayerLeaveFollowerCommands(const PlayerLeaveEvent& acEvent) noexcept
{
    if (acEvent.pPlayer)
    {
        m_followerCommandRates.erase(acEvent.pPlayer->GetId());
        m_followerClaimRates.erase(acEvent.pPlayer->GetId());
    }
}

void CharacterService::MakeFollowerOf(Player* apPlayer, const entt::entity aEntity, const bool aGrantAgain) const noexcept
{
    // A game reports only the followers it has loaded, so the actor is within the reporting player's loaded cells; and only a few a second,
    // as each claim resends the actor to everyone who sees her.
    const auto& cCell = m_world.get<CellIdComponent>(aEntity);
    if (!apPlayer->GetCellComponent() || !apPlayer->GetCellComponent().IsInRange(cCell, m_world.get<CharacterComponent>(aEntity).IsDragon()))
    {
        spdlog::debug("Follower: player {:X} said actor {:X} is theirs from out of range; ignored", apPlayer->GetId(), World::ToInteger(aEntity));
        return;
    }
    if (!m_followerClaimRates[apPlayer->GetId()].Allow(std::chrono::steady_clock::now()))
    {
        spdlog::debug("Follower: player {:X} reports followers too fast; ignored", apPlayer->GetId());
        return;
    }

    const auto& ownerComponent = m_world.get<OwnerComponent>(aEntity);
    const uint32_t cBefore = ownerComponent.FollowerOfPlayerId;
    TakeFollower(apPlayer, aEntity);
    const bool cMoved = GiveFollowerToPlayer(apPlayer, aEntity, cBefore, aGrantAgain);
    if (cMoved || ownerComponent.FollowerOfPlayerId != cBefore)
        ResendToViewers(aEntity);
}

void CharacterService::TakeFollower(Player* apPlayer, const entt::entity aEntity) const noexcept
{
    auto& ownerComponent = m_world.get<OwnerComponent>(aEntity);
    const uint32_t cPlayerId = apPlayer->GetId();
    const uint32_t cCurrent = ConnectedFollowerPlayer(m_world, ownerComponent);
    if (cCurrent == cPlayerId)
        return;

    // A body is nobody's follower: it goes to whoever grabs it.
    if (m_world.get<CharacterComponent>(aEntity).IsDead())
        return;

    // Followers in two players' saves: only the party leader's exists, and the other stays hidden in its own game.
    // A later change of leader moves nothing, since a follower only comes up again when her player's game reports her.
    if (cCurrent)
    {
        auto& partyService = m_world.GetPartyService();
        Player* pCurrent = m_world.GetPlayerManager().GetById(cCurrent);
        const auto* pParty = partyService.GetPlayerParty(apPlayer);
        if (!pParty || pParty != partyService.GetPlayerParty(pCurrent) || !partyService.IsPlayerLeader(apPlayer))
        {
            spdlog::info("Follower: actor {:X} stays player {:X}'s; player {:X} has her too, and hides theirs", World::ToInteger(aEntity), cCurrent, cPlayerId);
            return;
        }
    }

    ownerComponent.FollowerOfPlayerId = cPlayerId;
    spdlog::info("Follower: actor {:X} is now player {:X}'s{}", World::ToInteger(aEntity), cPlayerId, cCurrent ? ", the party leader, who has her too" : "");
}

bool CharacterService::GiveFollowerToPlayer(Player* apPlayer, const entt::entity aEntity, const uint32_t aPreviousFollower, const bool aGrantAgain) const noexcept
{
    auto& ownerComponent = m_world.get<OwnerComponent>(aEntity);
    if (ConnectedFollowerPlayer(m_world, ownerComponent) != apPlayer->GetId())
        return false;

    if (ownerComponent.GetOwner() == apPlayer)
    {
        if (aGrantAgain)
        {
            spdlog::info("Follower: actor {:X} granted again to player {:X}, whose game asked for her", World::ToInteger(aEntity), apPlayer->GetId());
            ownerComponent.SetOwner(nullptr);
            TransferOwnership(apPlayer, aEntity, OwnershipTransferReason::LeaderClaim);
        }
        return false;
    }

    // Refused by the rules for claims, as for a player in no party: the game that has her goes on running her.
    // Her player's game asks again once she is near, which then goes through if the rules let it.
    if (!CanClaimOwnership(apPlayer, aEntity, ownerComponent.OwnershipEpoch, OwnershipTransferReason::LeaderClaim, true))
    {
        ownerComponent.FollowerOfPlayerId = aPreviousFollower;
        return false;
    }

    return TransferOwnership(apPlayer, aEntity, OwnershipTransferReason::LeaderClaim);
}

void CharacterService::ResendToViewers(const entt::entity aEntity) const noexcept
{
    const auto& ownerComponent = m_world.get<OwnerComponent>(aEntity);
    const auto& characterComponent = m_world.get<CharacterComponent>(aEntity);
    const auto& cellIdComponent = m_world.get<CellIdComponent>(aEntity);
    const uint32_t cFollowerOf = ConnectedFollowerPlayer(m_world, ownerComponent);

    NotifyRemoveCharacter removeMessage;
    removeMessage.ServerId = World::ToInteger(aEntity);

    CharacterSpawnRequest spawnMessage;
    Serialize(m_world, aEntity, &spawnMessage);

    // The removal goes to everybody: a copy can stay at the edge of a player's loaded area after she walked out of their range.
    // The spawn goes to those in range, and the others are sent her when she comes into range.
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer == ownerComponent.GetOwner() || pPlayer->GetId() == cFollowerOf)
            continue;
        pPlayer->Send(removeMessage);
        if (pPlayer->GetCellComponent().IsInRange(cellIdComponent, characterComponent.IsDragon()))
            SendSpawn(m_world, pPlayer, aEntity, spawnMessage);
    }
}

void CharacterService::OnCharacterSpawned(const CharacterSpawnedEvent& acEvent) const noexcept
{
    CharacterSpawnRequest message;
    Serialize(m_world, acEvent.Entity, &message);

    const auto& ownerComp = m_world.get<OwnerComponent>(acEvent.Entity);
    if (!GameServer::Get()->SendToPlayersInRange(message, acEvent.Entity, ownerComp.GetOwner()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);

    GameServer::Get()->GetWorld().GetScriptService().HandleCharacterSpawn(acEvent.Entity);
}

void CharacterService::OnReferencesMoveRequest(const PacketEvent<ClientReferencesMoveRequest>& acMessage) const noexcept
{
    OwnerView<AnimationComponent, MovementComponent, CellIdComponent> view(m_world, acMessage.GetSender());

    auto& message = acMessage.Packet;

    acMessage.pPlayer->MarkMovement();

    // Relayed at once to every other player in range, with the owner's own tick: each receiving game times its playback
    // from when the owner captured the update, not from when the server passed it on.
    using Packer = MovementPacker<ServerReferencesMoveRequest>;
    thread_local TiltedPhoques::Buffer s_scratch(1 << 16);
    TiltedPhoques::Map<Player*, std::unique_ptr<Packer>> relays;
    const auto characterView = m_world.view<CharacterComponent>();

    for (auto& entry : message.Updates)
    {
        const auto entity = static_cast<entt::entity>(entry.first);

        auto itor = view.find(entity);
        if (itor == std::end(view))
        {
            spdlog::debug("{:x} requested move of {:x} but does not exist", acMessage.pPlayer->GetConnectionId(), World::ToInteger(entity));
            continue;
        }

        auto& movementComponent = view.get<MovementComponent>(*itor);
        auto& cellIdComponent = view.get<CellIdComponent>(*itor);

        // Movement arrives unreliably, so an older report can come after a newer one, and would set the actor back.
        // It is dropped; a step back of more than a second is a clock resync and is taken.
        constexpr uint64_t cReorderWindowMs = 1000;
        if (message.Tick < movementComponent.Tick && movementComponent.Tick - message.Tick < cReorderWindowMs)
            continue;

        auto& update = entry.second;
        auto& movement = update.UpdatedMovement;

        // A position outside the world, or a jump no actor makes, is not taken; a jump that stays is a teleport and is taken with the next report.
        if (GameServer::Get()->ValidatesClientReports())
        {
            const bool cMoved = cellIdComponent.Cell != movement.CellId || cellIdComponent.WorldSpaceId != movement.WorldSpaceId ||
                                !(cellIdComponent.CenterCoords == GridCellCoords::CalculateGridCellCoords(movement.Position.x, movement.Position.y));
            const auto cStep = ClientTrust::JudgeStep(
                {movementComponent.Position.x, movementComponent.Position.y, movementComponent.Position.z}, movementComponent.Tick,
                {movement.Position.x, movement.Position.y, movement.Position.z}, message.Tick, cMoved, movementComponent.HeldJump);
            if (cStep != ClientTrust::Step::kTake)
            {
                spdlog::debug(
                    "Refused a position of {:x} from player {}: {}", World::ToInteger(entity), acMessage.pPlayer->GetId(),
                    cStep == ClientTrust::Step::kHold ? "a jump beyond the game's speeds, held for the next report" : ClientTrust::RefusalName(ClientTrust::Refusal::kBadPosition));
                continue;
            }
        }

        movementComponent.Tick = message.Tick;

        const auto movementCopy = movementComponent;

        movementComponent.Position = movement.Position;
        movementComponent.Rotation = glm::vec3(movement.Rotation.x, 0.f, movement.Rotation.y);
        movementComponent.Variables = movement.Variables;
        movementComponent.Direction = movement.Direction;
        movementComponent.Flying = movement.Flying;
        movementComponent.VRPoseData = update.UpdatedVRPose;

        const CellIdComponent cCellBefore = cellIdComponent;
        cellIdComponent.Cell = movement.CellId;
        cellIdComponent.WorldSpaceId = movement.WorldSpaceId;
        cellIdComponent.CenterCoords = GridCellCoords::CalculateGridCellCoords(movement.Position.x, movement.Position.y);


        const auto cCharacter = characterView.find(entity);
        const bool cIsDragon = cCharacter != characterView.end() && characterView.get<CharacterComponent>(*cCharacter).IsDragon();
        const bool cIsPlayer = cCharacter != characterView.end() && characterView.get<CharacterComponent>(*cCharacter).IsPlayer();

        // A dead NPC's pose at rest is kept for the players sent its body later; an unposed report leaves it.
        if (cCharacter != characterView.end() && !cIsPlayer)
        {
            const VRPose& cPose = update.UpdatedVRPose;
            const bool cPosed = cPose.HasData && !cPose.NoBones;
            const bool cUsableRoot =
                cPose.HasRootPosition &&
                ClientTrust::IsUsableTransform(cPose.RootPosition[0], cPose.RootPosition[1], cPose.RootPosition[2], movement.Rotation.x, 0.f, movement.Rotation.y);
            switch (CorpseRestPose::OnReport(characterView.get<CharacterComponent>(*cCharacter).IsDead(), cPosed, cUsableRoot))
            {
            case CorpseRestPose::Report::kTake: m_restPoses.Take(entry.first, cPose); break;
            case CorpseRestPose::Report::kForget: m_restPoses.Forget(entry.first); break;
            case CorpseRestPose::Report::kIgnore: break;
            }
        }

        const bool cCellChanged = cCellBefore.Cell != cellIdComponent.Cell || cCellBefore.WorldSpaceId != cellIdComponent.WorldSpaceId ||
                                  !(cCellBefore.CenterCoords == cellIdComponent.CenterCoords);
        // An NPC whose own move takes it out of a player's range is removed from that player's view.
        // A move that brings it back sends it to them again.
        // Players' characters are handled by their own cell changes.
        if (cCellChanged && !cIsPlayer && cCharacter != characterView.end())
            UpdateViewsAfterMove(acMessage.pPlayer, entity, cCellBefore, cellIdComponent, cIsDragon);

        ReferenceUpdate relayed;
        relayed.UpdatedMovement = movement;
        relayed.UpdatedVRPose = update.UpdatedVRPose;
        const size_t cEntryBits = Packer::EntryBits(s_scratch, entry.first, relayed);

        // Players farther off get fewer of its updates (RelayThinning).
        const VRPose& cPose = update.UpdatedVRPose;
        const bool cCarriesKeptParts = cPose.HasData && (cPose.HasFingers || cPose.HasScale || cPose.HasWeapons || cPose.HasHandCheck);
        const bool cMidDue = RelayThinning::IsDue(message.Tick, movementComponent.MidRelayTick, RelayThinning::kMidIntervalMs);
        const bool cFarDue = RelayThinning::IsDue(message.Tick, movementComponent.FarRelayTick, RelayThinning::kFarIntervalMs);
        if (cMidDue)
            movementComponent.MidRelayTick = message.Tick;
        if (cFarDue)
            movementComponent.FarRelayTick = message.Tick;

        for (Player* pPlayer : m_world.GetPlayerManager())
        {
            if (pPlayer == acMessage.pPlayer || !cellIdComponent.IsInRange(pPlayer->GetCellComponent(), cIsDragon))
                continue;
            const auto cBand = RelayThinning::BandOf(DistanceFrom(m_world, pPlayer, cellIdComponent, movementComponent.Position));
            if (!RelayThinning::Relays(cBand, cMidDue, cFarDue, cCarriesKeptParts))
                continue;

            // Movement goes unreliably: a lost message costs only itself, and the next one is already on its way.
            auto& pPacker = relays[pPlayer];
            if (!pPacker)
                pPacker = std::make_unique<Packer>(message.Tick, s_scratch);
            ServerReferencesMoveRequest full;
            if (pPacker->Add(entry.first, relayed, cEntryBits, full))
                pPlayer->Send(full, TiltedPhoques::kUnreliable);
        }
    }

    // The last message to a player skips Nagle's wait, so the whole batch leaves now.
    for (auto it = relays.begin(); it != relays.end(); ++it)
        it->first->Send(it.value()->Current(), TiltedPhoques::kUnreliableNoNagle);
}

Vector<ActionEvent> CharacterService::KeepActions(const entt::entity aEntity, AnimationComponent& aAnimationComponent, const Vector<ActionEvent>& acActions) const noexcept
{
    Vector<ActionEvent> kept;
    for (const auto& action : acActions)
    {
        auto [canceled, reason] = GameServer::Get()->GetWorld().GetScriptService().HandleCharacterMove(aEntity);
        if (canceled)
            continue;

        aAnimationComponent.CurrentAction = action;
        kept.push_back(action);
    }

    // A player who comes into range later is sent these, so the actor starts in the right state.
    aAnimationComponent.ActionsReplayCache.AppendAll(kept);
    return kept;
}

void CharacterService::OnActionEventsRequest(const PacketEvent<RequestActionEvents>& acMessage) const noexcept
{
    OwnerView<AnimationComponent, CellIdComponent> view(m_world, acMessage.GetSender());
    const auto characterView = m_world.view<CharacterComponent>();

    // Relayed at once and reliably to every other player in range, as the movement is.
    TiltedPhoques::Map<Player*, NotifyActionEvents> relays;
    for (const auto& [serverId, actions] : acMessage.Packet.Actions)
    {
        const auto entity = static_cast<entt::entity>(serverId);
        auto itor = view.find(entity);
        if (itor == std::end(view))
        {
            spdlog::debug("{:x} sent actions of {:x}, which it does not own or which does not exist", acMessage.pPlayer->GetConnectionId(), serverId);
            continue;
        }

        const Vector<ActionEvent> kept = KeepActions(entity, view.get<AnimationComponent>(*itor), actions);
        if (kept.empty())
            continue;

        const auto& cellIdComponent = view.get<CellIdComponent>(*itor);
        const auto cCharacter = characterView.find(entity);
        const bool cIsDragon = cCharacter != characterView.end() && characterView.get<CharacterComponent>(*cCharacter).IsDragon();
        for (Player* pPlayer : m_world.GetPlayerManager())
        {
            if (pPlayer == acMessage.pPlayer || !cellIdComponent.IsInRange(pPlayer->GetCellComponent(), cIsDragon))
                continue;

            auto& relayed = relays[pPlayer].Actions[serverId];
            relayed.insert(relayed.end(), kept.begin(), kept.end());
        }
    }

    for (auto it = relays.begin(); it != relays.end(); ++it)
        it->first->Send(it->second);
}


void CharacterService::UpdateViewsAfterMove(const Player* apOwner, const entt::entity aEntity, const CellIdComponent& acBefore, const CellIdComponent& acNow,
                                            const bool aIsDragon) const noexcept
{
    const uint32_t cServerId = World::ToInteger(aEntity);
    // Only a door or a worldspace change removes it.
    // A walk along the edge of a player's loaded area would remove and re-create it again and again.
    // Outdoors, the copy there waits at that edge until the actor comes back.
    const bool cThroughDoor = acBefore.IsInInteriorCell() || acNow.IsInInteriorCell() || acBefore.WorldSpaceId != acNow.WorldSpaceId;
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer == apOwner)
            continue;

        const auto& cViewer = pPlayer->GetCellComponent();
        const bool cWasInRange = acBefore.IsInRange(cViewer, aIsDragon);
        const bool cIsInRange = acNow.IsInRange(cViewer, aIsDragon);
        if (cWasInRange == cIsInRange || (cWasInRange && !cThroughDoor))
            continue;

        if (cWasInRange)
        {
            // A reference of the game's own goes through the door in that player's game too, first.
            // Left where its copy last stood, it stays there in their world, as it last moved (a follower left swimming in a lake).
            const auto* pFormIdComponent = IsLivingFollower(m_world, aEntity) ? nullptr : m_world.try_get<FormIdComponent>(aEntity);
            if (pFormIdComponent)
            {
                const auto& cPosition = m_world.get<MovementComponent>(aEntity).Position;
                NotifyActorTeleport teleport{};
                teleport.FormId = pFormIdComponent->Id;
                teleport.WorldSpaceId = acNow.WorldSpaceId;
                teleport.CellId = acNow.Cell;
                teleport.Position.x = cPosition.x;
                teleport.Position.y = cPosition.y;
                teleport.Position.z = cPosition.z;
                pPlayer->Send(teleport);
            }

            NotifyRemoveCharacter removeMessage;
            removeMessage.ServerId = cServerId;
            pPlayer->Send(removeMessage);
            spdlog::info("CellRange: character {:X} moved from cell {:X} to {:X}, out of player {:X}'s range; removed from their view{}", cServerId, acBefore.Cell.BaseId,
                         acNow.Cell.BaseId, pPlayer->GetId(), pFormIdComponent ? ", its reference there moved through the door too" : "");
        }
        else
        {
            CharacterSpawnRequest spawnMessage;
            Serialize(m_world, aEntity, &spawnMessage);
            SendSpawn(m_world, pPlayer, aEntity, spawnMessage);
            spdlog::info("CellRange: character {:X} moved from cell {:X} to {:X}, into player {:X}'s range; sent to them", cServerId, acBefore.Cell.BaseId, acNow.Cell.BaseId,
                         pPlayer->GetId());
        }
    }
}

void CharacterService::OnFactionsChanges(const PacketEvent<RequestFactionsChanges>& acMessage) const noexcept
{
    OwnerView<CharacterComponent> view(m_world, acMessage.GetSender());

    auto& message = acMessage.Packet;

    for (auto& [id, factions] : message.Changes)
    {
        auto it = view.find(static_cast<entt::entity>(id));

        if (it == std::end(view) || view.get<OwnerComponent>(*it).GetOwner() != acMessage.pPlayer)
            continue;

        auto& characterComponent = view.get<CharacterComponent>(*it);
        characterComponent.FactionsContent = factions;
        characterComponent.SetDirtyFactions(true);
    }
}

void CharacterService::OnMountRequest(const PacketEvent<MountRequest>& acMessage) const noexcept
{
    const auto& message = acMessage.Packet;
    const entt::entity cRiderEntity = static_cast<entt::entity>(message.RiderId);
    const entt::entity cMountEntity = static_cast<entt::entity>(message.MountId);
    const auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent>();
    const auto& extensions = m_world.ctx().at<ExtensionService>();
    if (extensions.Reserved(message.RiderId) || extensions.Reserved(message.MountId))
        return;
    const auto riderIt = view.find(cRiderEntity);
    const auto mountIt = view.find(cMountEntity);

    if (riderIt == view.end() || mountIt == view.end() || cRiderEntity == cMountEntity)
    {
        spdlog::debug("Rejected mount request from player {:X} because rider {:X} or mount {:X} is invalid", acMessage.pPlayer->GetId(), message.RiderId, message.MountId);
        return;
    }

    const auto& riderOwner = view.get<OwnerComponent>(*riderIt);
    const auto& mountOwner = view.get<OwnerComponent>(*mountIt);
    const auto& riderCharacter = view.get<CharacterComponent>(*riderIt);
    const auto& mountCharacter = view.get<CharacterComponent>(*mountIt);
    const auto& riderCell = view.get<CellIdComponent>(*riderIt);
    const auto& mountCell = view.get<CellIdComponent>(*mountIt);
    auto& parties = m_world.GetPartyService();
    const auto* pParty = parties.GetPlayerParty(acMessage.pPlayer);
    MountSync::RequestState state{};
    state.RiderOwnedBySender = riderOwner.GetOwner() == acMessage.pPlayer;
    state.MountOwnedBySender = mountOwner.GetOwner() == acMessage.pPlayer;
    state.OwnersInSameParty = pParty && mountOwner.GetOwner() && pParty == parties.GetPlayerParty(mountOwner.GetOwner());
    state.SharingOn = bPartyHorses;
    state.RiderDead = riderCharacter.IsDead();
    state.MountDead = mountCharacter.IsDead();
    state.IsMount = mountCharacter.IsMount();
    state.MountIsPlayer = mountCharacter.IsPlayer();
    // Require compatible world/interior context in both directions. A sender near
    // the horse is not sufficient authority for a rider in another cell.
    state.PairInRange = riderCell && mountCell && riderCell.IsInRange(mountCell, mountCharacter.IsDragon()) && mountCell.IsInRange(riderCell, riderCharacter.IsDragon());
    state.SenderInRange = acMessage.pPlayer->GetCellComponent() && acMessage.pPlayer->GetCellComponent().IsInRange(mountCell, mountCharacter.IsDragon()) &&
                          acMessage.pPlayer->GetCellComponent().IsInRange(riderCell, riderCharacter.IsDragon());
    if (!MountSync::MayRequest(
            {message.RiderId, message.RiderOwnershipEpoch, message.MountId, message.MountOwnershipEpoch}, riderOwner.OwnershipEpoch, mountOwner.OwnershipEpoch, state))
    {
        spdlog::debug(
            "Rejected mount package from player {:X} for rider {:X} epoch {} and mount {:X} epoch {}", acMessage.pPlayer->GetId(), message.RiderId, message.RiderOwnershipEpoch,
            message.MountId, message.MountOwnershipEpoch);
        return;
    }

    if (!TransferOwnership(acMessage.pPlayer, *mountIt, OwnershipTransferReason::Mount))
        return;

    NotifyMount notify;
    notify.RiderId = message.RiderId;
    notify.MountId = message.MountId;
    notify.RiderOwnershipEpoch = riderOwner.OwnershipEpoch;
    notify.MountOwnershipEpoch = mountOwner.OwnershipEpoch;
    notify.IsRefresh = message.IsRefresh;

    // Kept for players sent the pair later (SendSpawn).
    MountSync::Ride ride{};
    ride.Mount = message.MountId;
    ride.RiderEpoch = riderOwner.OwnershipEpoch;
    ride.MountEpoch = mountOwner.OwnershipEpoch;
    ride.ToldAt = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    m_world.emplace_or_replace<RideComponent>(cRiderEntity, RideComponent{ride});

    if (!GameServer::Get()->SendToPlayersInRange(notify, cMountEntity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
    else
        spdlog::debug("Mount: rider {:X} requested package for {:X}; told the players in range", message.RiderId, message.MountId);
}

void CharacterService::OnDismountRequest(const PacketEvent<RequestDismount>& acMessage) const noexcept
{
    const auto& message = acMessage.Packet;
    const entt::entity cRiderEntity = static_cast<entt::entity>(message.RiderId);
    const auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent>();
    const auto riderIt = view.find(cRiderEntity);
    if (riderIt == view.end())
        return;

    const auto& riderOwner = view.get<OwnerComponent>(*riderIt);
    if (!MountSync::MayDismount(message.RiderOwnershipEpoch, riderOwner.OwnershipEpoch, riderOwner.GetOwner() == acMessage.pPlayer))
    {
        spdlog::debug("Rejected dismount from player {:X} for rider {:X} epoch {}", acMessage.pPlayer->GetId(), message.RiderId, message.RiderOwnershipEpoch);
        return;
    }

    if (const auto* pRide = m_world.try_get<RideComponent>(cRiderEntity); pRide && pRide->Ride.Mount == message.MountId)
        m_world.remove<RideComponent>(cRiderEntity);

    NotifyDismount notify;
    notify.RiderId = message.RiderId;
    notify.RiderOwnershipEpoch = message.RiderOwnershipEpoch;
    notify.MountId = message.MountId;

    if (!GameServer::Get()->SendToPlayersInRange(notify, cRiderEntity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void CharacterService::OnNewPackageRequest(const PacketEvent<NewPackageRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    // Only the game that runs the actor gives it a new package, as it alone sends its other changes.
    const entt::entity cOwned = static_cast<entt::entity>(message.ActorId);
    const auto cView = m_world.view<OwnerComponent>();
    const auto cFound = cView.find(cOwned);
    if (cFound == cView.end() || cView.get<OwnerComponent>(*cFound).GetOwner() != acMessage.pPlayer)
    {
        spdlog::debug("Ignored new package from player {:X} for actor {:X}: not theirs", acMessage.pPlayer->GetId(), message.ActorId);
        return;
    }

    NotifyNewPackage notify;
    notify.ActorId = message.ActorId;
    notify.PackageId = message.PackageId;

    const entt::entity cEntity = static_cast<entt::entity>(message.ActorId);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void CharacterService::OnRequestRespawn(const PacketEvent<RequestRespawn>& acMessage) const noexcept
{
    auto view = m_world.view<OwnerComponent, CharacterComponent>();
    auto it = view.find(static_cast<entt::entity>(acMessage.Packet.ActorId));
    if (it == view.end())
    {
        spdlog::warn("No OwnerComponent found for actor id {:X}", acMessage.Packet.ActorId);
        return;
    }

    auto& ownerComponent = view.get<OwnerComponent>(*it);

    // Replay cache needs to be cleared when a character respawns
    if (auto* pAnimationComponent = m_world.try_get<AnimationComponent>(*it))
        pAnimationComponent->ActionsReplayCache.Clear();

    if (ownerComponent.GetOwner() == acMessage.pPlayer)
    {
        if (!acMessage.Packet.AppearanceBuffer.empty())
        {
            auto& characterComponent = view.get<CharacterComponent>(*it);
            characterComponent.SaveBuffer = acMessage.Packet.AppearanceBuffer;
            characterComponent.ChangeFlags = acMessage.Packet.ChangeFlags;
            if (!acMessage.Packet.FaceTints.Entries.empty())
                characterComponent.FaceTints = acMessage.Packet.FaceTints;
            // Only a player's own character renames that player.
            if (!acMessage.Packet.Username.empty() && characterComponent.IsPlayer() && characterComponent.PlayerId == static_cast<int32_t>(acMessage.pPlayer->GetId()))
                m_world.GetPlayerService().Rename(*acMessage.pPlayer, acMessage.Packet.Username);
        }

        NotifyRespawn notify;
        notify.ActorId = acMessage.Packet.ActorId;

        if (!GameServer::Get()->SendToPlayersInRange(notify, *it, acMessage.GetSender()))
            spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
    }
    else
    {
        CharacterSpawnRequest message;
        Serialize(m_world, *it, &message);

        SendSpawn(m_world, acMessage.GetSender(), *it, message);
    }
}

void CharacterService::OnSyncExperienceRequest(const PacketEvent<SyncExperienceRequest>& acMessage) const noexcept
{
    if (!bEnableXpSync)
        return;

    NotifySyncExperience notify;
    notify.Experience = acMessage.Packet.Experience;

    const auto& partyComponent = acMessage.pPlayer->GetParty();
    GameServer::Get()->SendToParty(notify, partyComponent, acMessage.GetSender());
}

void CharacterService::OnDialogueRequest(const PacketEvent<DialogueRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    // Anyone near an NPC may start its dialogue; its owner's game plays it.
    if (GameServer::Get()->ValidatesClientReports())
    {
        const auto cSpeaker = ClientTrust::FactsOf(m_world, message.ServerId, acMessage.pPlayer);
        if (const auto cRefusal = ClientTrust::JudgeSpeech(cSpeaker.Known, cSpeaker.SenderOwns, cSpeaker.InRange, message.SoundFilename.size());
            cRefusal != ClientTrust::Refusal::kNone)
        {
            spdlog::debug(
                "Refused a voice line of {:X} from player {}: {}", message.ServerId, acMessage.pPlayer ? acMessage.pPlayer->GetId() : 0, ClientTrust::RefusalName(cRefusal));
            return;
        }
    }

    NotifyDialogue notify{};
    notify.ServerId = message.ServerId;
    notify.SoundFilename = message.SoundFilename;
    notify.SceneId = message.SceneId;
    notify.ScenePhase = message.ScenePhase;

    const entt::entity cEntity = static_cast<entt::entity>(message.ServerId);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void CharacterService::OnSubtitleRequest(const PacketEvent<SubtitleRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    if (GameServer::Get()->ValidatesClientReports())
    {
        const auto cSpeaker = ClientTrust::FactsOf(m_world, message.ServerId, acMessage.pPlayer);
        if (const auto cRefusal = ClientTrust::JudgeSpeech(cSpeaker.Known, cSpeaker.SenderOwns, cSpeaker.InRange, message.Text.size()); cRefusal != ClientTrust::Refusal::kNone)
        {
            spdlog::debug(
                "Refused a subtitle of {:X} from player {}: {}", message.ServerId, acMessage.pPlayer ? acMessage.pPlayer->GetId() : 0, ClientTrust::RefusalName(cRefusal));
            return;
        }
    }

    NotifySubtitle notify{};
    notify.ServerId = message.ServerId;
    notify.Text = message.Text;
    notify.SceneId = message.SceneId;
    notify.ScenePhase = message.ScenePhase;

    const entt::entity cEntity = static_cast<entt::entity>(message.ServerId);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void CharacterService::CreateCharacter(const PacketEvent<AssignCharacterRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    const auto gameId = message.ReferenceId;
    const auto baseId = message.FormId;

    const auto cEntity = m_world.create();
    const auto isTemporary = gameId.ModId == std::numeric_limits<uint32_t>::max();
    const auto isPlayer = (gameId.ModId == 0 && gameId.BaseId == 0x14);
    const auto isCustom = isPlayer || isTemporary;

    // For player characters and temporary forms
    if (!isCustom)
    {
        m_world.emplace<FormIdComponent>(cEntity, gameId.BaseId, gameId.ModId);
    }
    else if (baseId != GameId{} && !isTemporary)
    {
        m_world.destroy(cEntity);
        spdlog::warn("Unexpected NpcId, player {:x} might be forging packets", acMessage.pPlayer->GetConnectionId());
        return;
    }

    auto* const pServer = GameServer::Get();

    m_world.emplace<OwnerComponent>(cEntity, acMessage.pPlayer);

    auto& cellIdComponent = m_world.emplace<CellIdComponent>(cEntity, message.CellId);
    if (message.WorldSpaceId != GameId{})
    {
        cellIdComponent.WorldSpaceId = message.WorldSpaceId;
        cellIdComponent.CenterCoords = GridCellCoords::CalculateGridCellCoords(message.Position);
    }

    auto& characterComponent = m_world.emplace<CharacterComponent>(cEntity);
    characterComponent.ChangeFlags = message.ChangeFlags;
    characterComponent.SaveBuffer = std::move(message.AppearanceBuffer);
    characterComponent.BaseId = FormIdComponent(message.FormId);
    // Client-authoritative like BaseId; worst case a forged id changes which NPC identity renders.
    if (message.LeveledNpcPickId != GameId{})
        characterComponent.LeveledNpcPickId = FormIdComponent(message.LeveledNpcPickId);

    if (characterComponent.LeveledNpcPickId)
        spdlog::debug("Stored leveled NPC pick {:x}:{:x} for FormId {:x}:{:x}", message.LeveledNpcPickId.ModId, message.LeveledNpcPickId.BaseId, gameId.ModId, gameId.BaseId);
    characterComponent.FaceTints = message.FaceTints;
    characterComponent.FactionsContent = message.FactionsContent;
    characterComponent.SetDead(message.CurrentActorData.IsDead);
    // Announced by a game that saw it come back from a death (ActorValueService::OnDeathStateChange): announced again, it keeps that.
    characterComponent.SetRevivedSinceDeath(!message.CurrentActorData.IsDead && message.CurrentActorData.RevivedSinceDeath);
    characterComponent.SetPlayer(isPlayer);
    if (isPlayer)
        characterComponent.SpawnGeneration = ++s_playerSpawnGeneration;
    characterComponent.SetWeaponDrawn(message.CurrentActorData.IsWeaponDrawn);
    characterComponent.SetDragon(message.IsDragon);
    characterComponent.SetMount(message.IsMount);
    characterComponent.SetPlayerSummon(message.IsPlayerSummon);

    // Where and when a game spawned it, so that another game's spawn for the same event can be paired with it.
    if (isTemporary && !message.IsPlayerSummon)
    {
        auto& scriptSpawnComponent = m_world.emplace<ScriptSpawnComponent>(cEntity);
        scriptSpawnComponent.CellId = message.CellId;
        scriptSpawnComponent.WorldSpaceId = message.WorldSpaceId;
        scriptSpawnComponent.SpawnPosition = message.Position;
        scriptSpawnComponent.SpawnedAt = std::chrono::steady_clock::now();
    }

    // Known before anyone else is sent the character, so no other game ever shows her on its own reference.
    if (!isCustom && message.IsFollower && !message.CurrentActorData.IsDead)
    {
        m_world.get<OwnerComponent>(cEntity).FollowerOfPlayerId = acMessage.pPlayer->GetId();
        spdlog::info("Follower: actor {:X} is now player {:X}'s", World::ToInteger(cEntity), acMessage.pPlayer->GetId());
    }

    auto& inventoryComponent = m_world.emplace<InventoryComponent>(cEntity);
    inventoryComponent.Content = message.CurrentActorData.InitialInventory;

    auto& actorValuesComponent = m_world.emplace<ActorValuesComponent>(cEntity);
    actorValuesComponent.CurrentActorValues = message.CurrentActorData.InitialActorValues;

    spdlog::debug("FormId: {:x}:{:x} - NpcId: {:x}:{:x} assigned to {:x}", gameId.ModId, gameId.BaseId, baseId.ModId, baseId.BaseId, acMessage.pPlayer->GetConnectionId());

    auto& movementComponent = m_world.emplace<MovementComponent>(cEntity);
    movementComponent.Tick = pServer->GetTick();
    movementComponent.Position = message.Position;
    movementComponent.Rotation = {message.Rotation.x, 0.f, message.Rotation.y};

    m_world.emplace<AnimationComponent>(cEntity);

    // If this is a player character store a ref and trigger an event
    if (isPlayer)
    {
        const auto pPlayer = acMessage.pPlayer;

        pPlayer->SetCharacter(cEntity);
        pPlayer->GetQuestLogComponent().QuestContent = message.QuestContent;
        characterComponent.PlayerId = pPlayer->GetId();

        auto& dispatcher = m_world.GetDispatcher();
        dispatcher.trigger(PlayerEnterWorldEvent(pPlayer));
    }

    AssignCharacterResponse response{};
    response.Cookie = message.Cookie;
    response.Owner = true;
    PopulateAssignmentResponse(cEntity, response);

    pServer->Send(acMessage.pPlayer->GetConnectionId(), response);

    auto& dispatcher = m_world.GetDispatcher();
    dispatcher.trigger(CharacterSpawnedEvent(cEntity));
}

void CharacterService::PopulateAssignmentResponse(const entt::entity aEntity, AssignCharacterResponse& aResponse) const noexcept
{
    aResponse.ServerId = World::ToInteger(aEntity);

    if (const auto* pOwnerComponent = m_world.try_get<OwnerComponent>(aEntity))
    {
        aResponse.OwnershipEpoch = pOwnerComponent->OwnershipEpoch;
        aResponse.FollowerOfPlayerId = ConnectedFollowerPlayer(m_world, *pOwnerComponent);
    }

    if (const auto* pActorValuesComponent = m_world.try_get<ActorValuesComponent>(aEntity))
        aResponse.AllActorValues = pActorValuesComponent->CurrentActorValues;

    if (const auto* pInventoryComponent = m_world.try_get<InventoryComponent>(aEntity))
        aResponse.CurrentInventory = pInventoryComponent->Content;

    if (const auto* pCharacterComponent = m_world.try_get<CharacterComponent>(aEntity))
    {
        aResponse.FactionsContent = pCharacterComponent->FactionsContent;
        aResponse.PlayerId = pCharacterComponent->PlayerId;
        aResponse.IsDead = pCharacterComponent->IsDead();
        aResponse.RevivedSinceDeath = pCharacterComponent->IsRevivedSinceDeath();
        aResponse.DismemberedLimbs = pCharacterComponent->DismemberedLimbs;
        aResponse.IsWeaponDrawn = pCharacterComponent->IsWeaponDrawn();
        aResponse.LeveledNpcPickId = pCharacterComponent->LeveledNpcPickId.Id;

        if (pCharacterComponent->LeveledNpcPickId)
        {
            spdlog::debug(
                "Including leveled NPC pick in assignment response for actor {:X}, pick: {:x}:{:x}, owner: {}, epoch: {}",
                aResponse.ServerId,
                aResponse.LeveledNpcPickId.ModId,
                aResponse.LeveledNpcPickId.BaseId,
                aResponse.Owner,
                aResponse.OwnershipEpoch);
        }
    }

    if (const auto* pMovementComponent = m_world.try_get<MovementComponent>(aEntity))
        aResponse.Position = pMovementComponent->Position;

    if (const auto* pCellIdComponent = m_world.try_get<CellIdComponent>(aEntity))
    {
        aResponse.CellId = pCellIdComponent->Cell;
        aResponse.WorldSpaceId = pCellIdComponent->WorldSpaceId;
    }

    if (auto* pAnimationComponent = m_world.try_get<AnimationComponent>(aEntity))
        aResponse.ActionsToReplay = pAnimationComponent->ActionsReplayCache.FormRefinedReplayChain();
}

void CharacterService::NoteCrimeWitness(const entt::entity aWitness, const uint32_t aPlayerId, const uint32_t aOwnershipEpoch) noexcept
{
    if (bCrimeGuardHandoff)
        m_crimeWitnesses.Note(World::ToInteger(aWitness), aPlayerId, aOwnershipEpoch, std::chrono::steady_clock::now());
}

const char* CharacterService::GetOwnershipTransferReasonName(const OwnershipTransferReason aReason) noexcept
{
    switch (aReason)
    {
    case OwnershipTransferReason::LeaderAssignment:
        return "party leader assignment";
    case OwnershipTransferReason::LeaderClaim:
        return "party leader claim";
    case OwnershipTransferReason::NearestMember: return "party member near it while the leader is far";
    case OwnershipTransferReason::Mount:
        return "mounting";
    case OwnershipTransferReason::Relinquish:
        return "owner relinquished control";
    case OwnershipTransferReason::OwnerUnavailable:
        return "owner became unavailable";
    case OwnershipTransferReason::KillingBlow:
        return "killing blow on a protected NPC";
    case OwnershipTransferReason::CrimeWitness: return "witness of the claimant's crime";
    case OwnershipTransferReason::Hold: return "grab of a living NPC";
    case OwnershipTransferReason::HoldEnded: return "end of a grab";
    }

    return "unknown reason";
}

bool CharacterService::CanClaimOwnership(Player* apPlayer, const entt::entity aEntity, const uint32_t aExpectedOwnershipEpoch, const OwnershipTransferReason aReason,
                                         const bool aFollower) const noexcept
{
    const uint32_t serverId = World::ToInteger(aEntity);
    const char* pReasonName = GetOwnershipTransferReasonName(aReason);
    const auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent>();
    const auto it = view.find(aEntity);
    if (it == view.end())
    {
        spdlog::info("Rejected {} from player {:X} because actor {:X} is unavailable", pReasonName, apPlayer->GetId(), serverId);
        return false;
    }
    // A temporary actor (no form id on the server) can be claimed only as a dead body, so others can drag it.
    // Or for a killing blow, which makes it one.
    if (!m_world.try_get<FormIdComponent>(aEntity) && !view.get<CharacterComponent>(*it).IsDead() && aReason != OwnershipTransferReason::KillingBlow)
    {
        spdlog::info("Rejected {} from player {:X} because actor {:X} is temporary and alive", pReasonName, apPlayer->GetId(), serverId);
        return false;
    }

    const auto& ownerComponent = view.get<OwnerComponent>(*it);
    const auto& characterComponent = view.get<CharacterComponent>(*it);
    const auto& cellIdComponent = view.get<CellIdComponent>(*it);
    Player* const pCurrentOwner = ownerComponent.GetOwner();
    const uint32_t currentOwnerId = pCurrentOwner ? pCurrentOwner->GetId() : 0;

    // Logged at info so a refused claim on a dragged body says why (live-body-grab).
    const auto reject = [&](const char* apReason)
    {
        spdlog::info("Rejected {} from player {:X} for {} {:X}: {} (requested epoch {}, current owner {:X}, current epoch {})", pReasonName, apPlayer->GetId(),
                     characterComponent.IsDead() ? "dead body" : "actor", serverId, apReason, aExpectedOwnershipEpoch, currentOwnerId, ownerComponent.OwnershipEpoch);
        return false;
    };

    if (aExpectedOwnershipEpoch == 0 || ownerComponent.OwnershipEpoch != aExpectedOwnershipEpoch)
        return reject("the ownership epoch is stale");

    if (!pCurrentOwner || pCurrentOwner == apPlayer)
        return reject("the player already owns the actor");

    if (characterComponent.IsMount() || characterComponent.IsPlayer())
        return reject("the actor cannot be claimed");

    // Another player's grab keeps the NPC while it holds it (GrabHold); a killing blow may still take it.
    if (const auto hold = m_holds.find(serverId); hold != m_holds.end() && GrabHold::BlocksClaim(
                                                                               GrabHold::IsLive(hold->second, currentOwnerId, ownerComponent.OwnershipEpoch),
                                                                               hold->second.Holder == apPlayer->GetId(), aReason == OwnershipTransferReason::KillingBlow))
        return reject("another player holds it");

    // Not for the leader's claim: its game claims only actors loaded next to it, wherever the server last had them.
    if (aReason != OwnershipTransferReason::LeaderClaim && !apPlayer->GetCellComponent().IsInRange(cellIdComponent, characterComponent.IsDragon()))
        return reject("the actor is out of range");

    // A living follower stays with its player's game while that player is connected, whoever else has her in a save.
    // Her body goes to whoever grabs it, as anybody's does.
    if (const uint32_t cFollowerOf = ConnectedFollowerPlayer(m_world, ownerComponent); !characterComponent.IsDead() && cFollowerOf && cFollowerOf != apPlayer->GetId())
        return reject("it is another player's follower");

    // A flying dragon stays with its owner until it lands or dies; flight is not handed over and it would fall.
    if (characterComponent.IsDragon() && pCurrentOwner && pCurrentOwner != apPlayer)
        if (const auto* pMovement = m_world.try_get<MovementComponent>(aEntity); pMovement && pMovement->Flying)
            return reject("it is a dragon flying in its owner's game");

    // A player's killing blow on a protected NPC: the game lets only its own player kill one, so it goes to the killer's game.
    // Party or not, leader or not; another player's follower was refused above.
    if (aReason == OwnershipTransferReason::KillingBlow)
        return characterComponent.IsDead() ? reject("it is already dead") : true;

    // A witness the claimant's own crime report named lately (CrimeReports::ServerGivesWitness): its arrest belongs to the offender's game.
    if (aReason == OwnershipTransferReason::CrimeWitness)
        return characterComponent.IsDead() ? reject("it is dead") : true;

    // A dead body goes to whoever grabs it, party or not, since only the owner sends its bones.
    // The epoch check above settles two players grabbing at once.
    // A game that only registers it gets a copy: taking it would make that game's corpse the truth for everyone.
    const bool cDeadBody = characterComponent.IsDead();
    if (cDeadBody)
        return aReason == OwnershipTransferReason::LeaderAssignment ? reject("a dead body changes hands only when it is grabbed") : true;

    auto& partyService = m_world.GetPartyService();
    const bool cNearestMember = aReason == OwnershipTransferReason::NearestMember;
    if (!partyService.IsPlayerInParty(apPlayer) || (!cDeadBody && !aFollower && !cNearestMember && !partyService.IsPlayerLeader(apPlayer)))
        return reject(cDeadBody || aFollower || cNearestMember ? "the player is not in a party" : "the player is not the party leader");

    PartyService::Party* const pParty = partyService.GetPlayerParty(apPlayer);
    if (!pParty || std::find(pParty->Members.begin(), pParty->Members.end(), pCurrentOwner) == pParty->Members.end())
        return reject("the current owner is not in the party");

    // The leader's game takes an NPC near its player; one far from it goes to a member near it instead (OwnerSpread).
    const auto* pMovement = m_world.try_get<MovementComponent>(aEntity);
    if (pMovement && aReason == OwnershipTransferReason::LeaderAssignment && !aFollower)
    {
        const NearestMember cNearest =
            FindNearestMember(m_world, *pParty, aEntity, ownerComponent, cellIdComponent, pMovement->Position, characterComponent.IsDragon(), std::chrono::steady_clock::now());
        // Asked for every actor the leader's game registers, so not logged as a refusal.
        if (!OwnerSpread::LeaderTakes(DistanceFrom(m_world, apPlayer, cellIdComponent, pMovement->Position), cNearest.Distance))
        {
            spdlog::debug(
                "Actor {:X} stays with player {:X}'s game: party member {:X} is near it and the leader {:X} is not", serverId, currentOwnerId,
                cNearest.pPlayer ? cNearest.pPlayer->GetId() : 0, apPlayer->GetId());
            return false;
        }
    }
    if (cNearestMember && (!pMovement || pCurrentOwner->GetId() != pParty->LeaderPlayerId ||
                           !OwnerSpread::MemberTakesFromLeader(
                               DistanceFrom(m_world, pCurrentOwner, cellIdComponent, pMovement->Position), DistanceFrom(m_world, apPlayer, cellIdComponent, pMovement->Position))))
        return reject("the leader is near it, or the player is not");

    return true;
}

bool CharacterService::TransferOwnership(Player* apPlayer, const entt::entity aEntity, const OwnershipTransferReason aReason, const bool aResetInvalidOwners) const noexcept
{
    const char* pReasonName = GetOwnershipTransferReasonName(aReason);
    const auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent>();
    const auto it = view.find(aEntity);
    if (!apPlayer || it == view.end())
    {
        spdlog::warn("Cannot transfer ownership of actor {:X} for {} because the target is invalid", World::ToInteger(aEntity), pReasonName);
        return false;
    }

    auto& ownerComponent = view.get<OwnerComponent>(*it);
    Player* const pOldOwner = ownerComponent.GetOwner();
    if (pOldOwner == apPlayer)
        return true;

    const uint32_t oldOwnerId = pOldOwner ? pOldOwner->GetId() : 0;
    const uint32_t oldEpoch = ownerComponent.OwnershipEpoch;
    uint32_t newEpoch = oldEpoch + 1;
    if (newEpoch == 0)
        newEpoch = 1;

    NotifyOwnershipTransfer notify{};
    notify.ServerId = World::ToInteger(aEntity);
    notify.OwnerPlayerId = apPlayer->GetId();
    notify.OwnershipEpoch = newEpoch;
    notify.CurrentActorData = BuildActorData(aEntity);
    notify.LeveledNpcPickId = view.get<CharacterComponent>(*it).LeveledNpcPickId.Id;
    // The fight its last owner reported, so the new owner's game carries it on instead of starting the character's AI afresh.
    notify.CombatTarget = m_world.GetPresenceService().FightOf(aEntity);
    // Where its last owner's game last had it, so the others do not see it step back to where the new owner's copy was.
    // Taken before the hand-over clears the tick below; a zero tick means that game sent nothing since it was given the actor.
    if (const auto* pMovement = m_world.try_get<MovementComponent>(aEntity); pMovement && pMovement->Tick)
    {
        const auto& cellIdComponent = view.get<CellIdComponent>(*it);
        notify.CellId = cellIdComponent.Cell;
        notify.Position = pMovement->Position;
        notify.Heading = pMovement->Rotation.z;
    }

    ownerComponent.SetOwner(apPlayer);
    ownerComponent.OwnershipEpoch = newEpoch;
    // The new owner's reports are ordered on its own clock, which may run a little behind the old owner's.
    if (auto* pMovement = m_world.try_get<MovementComponent>(aEntity))
        pMovement->Tick = 0;
    if (aResetInvalidOwners)
        ownerComponent.InvalidOwners.clear();

    if (!GameServer::Get()->SendToPlayersInRange(notify, aEntity, pOldOwner))
        spdlog::error("Failed to broadcast ownership transfer for actor {:X}", notify.ServerId);

    // The former owner may already be out of range, so notify it directly as well.
    if (pOldOwner)
        pOldOwner->Send(notify);

    // Also tell the new owner directly when the actor is out of its range (a leader's claim), or it never sends for it.
    if (apPlayer != pOldOwner && !apPlayer->GetCellComponent().IsInRange(view.get<CellIdComponent>(*it), view.get<CharacterComponent>(*it).IsDragon()))
        apPlayer->Send(notify);

    spdlog::info(
        "Transferred ownership of actor {:X} from player {:X} to player {:X} for {} (epoch {} to {}){}",
        notify.ServerId, oldOwnerId, notify.OwnerPlayerId, pReasonName, oldEpoch, newEpoch,
        notify.CombatTarget ? fmt::format("; it fights actor {:X}", notify.CombatTarget) : std::string());

    return true;
}

void CharacterService::TransferToNextOwner(const entt::entity aEntity, const OwnershipTransferReason aReason) const noexcept
{
    const char* pReasonName = GetOwnershipTransferReasonName(aReason);
    const auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent>();
    const auto it = view.find(aEntity);
    if (it == view.end())
    {
        spdlog::warn("Cannot select a new owner for actor {:X} after {} because the actor is missing", World::ToInteger(aEntity), pReasonName);
        return;
    }

    auto& ownerComponent = view.get<OwnerComponent>(*it);
    const auto& characterComponent = view.get<CharacterComponent>(*it);
    const auto& cellIdComponent = view.get<CellIdComponent>(*it);

    // A living follower is never run by another game.
    // Left behind, she stays her player's while somebody can see her: her copy stands there until her game takes her up again when she loads.
    // With her player gone, or nobody near, she is removed, and each game has its own reference again.
    const Player* pOwner = ownerComponent.GetOwner();
    if (pOwner && !characterComponent.IsDead() && ConnectedFollowerPlayer(m_world, ownerComponent) == pOwner->GetId())
    {
        bool seen = false;
        for (Player* pPlayer : m_world.GetPlayerManager())
            seen = seen || (pPlayer != pOwner && pPlayer->GetCellComponent().IsInRange(cellIdComponent, characterComponent.IsDragon()));
        if (aReason == OwnershipTransferReason::Relinquish && seen)
        {
            spdlog::info("Follower: actor {:X} left behind by player {:X}; stays theirs and stands where she is", World::ToInteger(aEntity), pOwner->GetId());
            return;
        }
        spdlog::info("Follower: actor {:X} removed after {}: nobody else runs player {:X}'s follower", World::ToInteger(aEntity), pReasonName, pOwner->GetId());
        m_world.GetDispatcher().trigger(CharacterRemoveEvent(World::ToInteger(aEntity)));
        return;
    }

    // Declined owners are kept, so the actor cannot bounce between unloaded clients.
    const auto cNow = std::chrono::steady_clock::now();
    auto& presence = m_world.GetPresenceService();
    const bool cLogs = spdlog::should_log(spdlog::level::debug);
    for (Player* pPlayer : presence.NextOwners(aEntity, ownerComponent, cellIdComponent, characterComponent.IsDragon(), cNow))
    {
        // Which kind of candidate took it (NextOwners' order), read before the handoff.
        const auto cKind = cLogs ? presence.LoadedAs(aEntity, pPlayer->GetId(), cNow) : std::nullopt;
        if (!TransferOwnership(pPlayer, aEntity, aReason, false))
            continue;
        if (cLogs)
        {
            // How many other living characters its game already runs.
            const char* pHas = !cKind ? "in range only" : *cKind == ActorPresenceKind::StandIn ? "on a stand-in" : "loaded";
            size_t owned = 0;
            const auto cOwned = m_world.view<OwnerComponent, CharacterComponent>();
            for (const auto cOther : cOwned)
            {
                const auto& cCharacter = cOwned.get<CharacterComponent>(cOther);
                owned += cOther != aEntity && !cCharacter.IsPlayer() && !cCharacter.IsDead() && cOwned.get<OwnerComponent>(cOther).GetOwner() == pPlayer ? 1 : 0;
            }
            spdlog::debug(
                "Handoff tier: actor {:X} after {} to player {:X}, whose game has it {} and ran {} other character(s)", World::ToInteger(aEntity), pReasonName, pPlayer->GetId(),
                pHas, owned);
        }
        return;
    }

    spdlog::info("Removing actor {:X} after {} because no eligible owner remains", World::ToInteger(aEntity), pReasonName);
    m_world.GetPresenceService().LogRemovalWhileSeen(aEntity, pReasonName);
    m_world.GetDispatcher().trigger(CharacterRemoveEvent(World::ToInteger(aEntity)));
}

ActorData CharacterService::BuildActorData(const entt::entity acEntity) const noexcept
{
    ActorData actorData{};

    const auto* pActorValuesComponent = m_world.try_get<ActorValuesComponent>(acEntity);
    if (pActorValuesComponent)
    {
        actorData.InitialActorValues = pActorValuesComponent->CurrentActorValues;
    }

    const auto* pInventoryComponent = m_world.try_get<InventoryComponent>(acEntity);
    if (pInventoryComponent)
    {
        actorData.InitialInventory = pInventoryComponent->Content;
    }

    actorData.IsDead = false;
    const auto* pCharacterComponent = m_world.try_get<CharacterComponent>(acEntity);
    if (pCharacterComponent)
    {
        actorData.IsDead = pCharacterComponent->IsDead();
        actorData.IsWeaponDrawn = pCharacterComponent->IsWeaponDrawn();
        actorData.RevivedSinceDeath = pCharacterComponent->IsRevivedSinceDeath();
    }

    return actorData;
}

void CharacterService::ProcessFactionsChanges() const noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 2000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    const auto characterView = m_world.view<CellIdComponent, CharacterComponent, OwnerComponent>();

    TiltedPhoques::Map<Player*, NotifyFactionsChanges> messages;

    for (auto entity : characterView)
    {
        auto& characterComponent = characterView.get<CharacterComponent>(entity);
        auto& cellIdComponent = characterView.get<CellIdComponent>(entity);
        auto& ownerComponent = characterView.get<OwnerComponent>(entity);

        // If we have nothing new to send skip this
        if (!characterComponent.IsDirtyFactions())
            continue;

        for (auto pPlayer : m_world.GetPlayerManager())
        {
            if (pPlayer == ownerComponent.GetOwner())
                continue;

            if (!cellIdComponent.IsInRange(pPlayer->GetCellComponent(), characterComponent.IsDragon()))
                continue;

            auto& message = messages[pPlayer];
            auto& change = message.Changes[World::ToInteger(entity)];

            change = characterComponent.FactionsContent;
        }

        characterComponent.SetDirtyFactions(false);
    }

    for (auto [pPlayer, message] : messages)
    {
        if (!message.Changes.empty())
            pPlayer->Send(message);
    }
}

void CharacterService::OnClashRequest(const PacketEvent<ClashRequest>& acMessage) const noexcept
{
    const ClashRequest& packet = acMessage.Packet;
    const auto character = acMessage.pPlayer->GetCharacter();
    const auto other = static_cast<entt::entity>(packet.OtherId);
    if (!character || !m_world.valid(other))
        return;
    if (!ClientTrust::IsInsideWorld({packet.Point.x, packet.Point.y, packet.Point.z}) || !ClientTrust::IsSaneValue(packet.Speed))
        return;

    NotifyClash notify{};
    notify.FromId = World::ToInteger(*character);
    notify.OtherId = packet.OtherId;
    notify.OtherSide = packet.OtherSide;
    notify.OwnSide = packet.OwnSide;
    notify.Point = packet.Point;
    notify.Speed = packet.Speed;
    notify.Tick = packet.Tick;
    // To everyone near the weapon that was met, its owner included, but not back to the sender.
    if (!GameServer::Get()->SendToPlayersInRange(notify, other, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void CharacterService::OnArrowNockedRequest(const PacketEvent<ArrowNockedRequest>& acMessage) const noexcept
{
    // Always the sender's own character: a player can only say what is on their own bow.
    const auto character = acMessage.pPlayer->GetCharacter();
    if (!character || !m_world.valid(*character))
        return;

    NotifyArrowNocked notify{};
    notify.ServerId = World::ToInteger(*character);
    notify.Nocked = acMessage.Packet.Nocked;
    notify.InHand = acMessage.Packet.InHand;
    notify.HandSide = acMessage.Packet.HandSide;
    notify.HandOffset = acMessage.Packet.HandOffset;
    notify.HandRotation = acMessage.Packet.HandRotation;
    if (!GameServer::Get()->SendToPlayersInRange(notify, *character, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}
