#pragma once

#include <Events/PacketEvent.h>
#include <Structs/ActorData.h>
#include <Structs/ActionEvent.h>
#include <Messages/ServerReferencesMoveRequest.h>
#include <FollowerCommand.h>
#include <GrabHold.h>
#include <CrimeReports.h>
#include <CorpseRestPose.h>

struct UpdateEvent;
struct CharacterInteriorCellChangeEvent;
struct CharacterSpawnedEvent;
struct World;
struct AssignCharacterRequest;
struct RequestFollowerCommand;
struct RequestStuckArrow;
struct RequestDismember;
struct PlayerLeaveEvent;
struct AssignCharacterResponse;
struct CharacterSpawnRequest;
struct ClientReferencesMoveRequest;
struct RequestFactionsChanges;
struct GridCellCoords;
struct RequestOwnershipTransfer;
struct CharacterRemoveEvent;
struct CharacterExteriorCellChangeEvent;
struct RequestOwnershipClaim;
struct RequestFollowerState;
struct OwnershipTransferEvent;
struct MountRequest;
struct RequestDismount;
struct NewPackageRequest;
struct RequestRespawn;
struct ClashRequest;
struct ArrowNockedRequest;
struct SyncExperienceRequest;
struct DialogueRequest;
struct SubtitleRequest;
struct Player;
struct CellIdComponent;
struct OwnerComponent;
struct RequestActionEvents;
struct ReferenceUpdate;
struct AnimationComponent;

/**
 * @brief Manages player and actor state.
 */
struct CharacterService
{
    CharacterService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~CharacterService() noexcept = default;

    TP_NOCOPYMOVE(CharacterService);

    bool IsHeld(uint32_t aActor) const noexcept;

    static void Serialize(World& aRegistry, entt::entity aEntity, CharacterSpawnRequest* apSpawnRequest) noexcept;
    //! Sends a character to a player, and the ride it or its horse is on, so that its copy is seated at once.
    static void SendSpawn(World& aWorld, Player* apPlayer, entt::entity aEntity, const CharacterSpawnRequest& acSpawn) noexcept;
    //! A crime report from this player named this NPC, at aOwnershipEpoch, as a witness: for a while, the player's game may claim it for the
    //! crime at that epoch (CrimeReports::ServerGivesWitness).
    void NoteCrimeWitness(entt::entity aWitness, uint32_t aPlayerId, uint32_t aOwnershipEpoch) noexcept;
    //! Sends a player the pose a dead NPC's body came to rest in at its owner's, for its copy to lie in it (CorpseRestPose).
    //! Nothing when none is kept.
    void SendRestPose(Player* apPlayer, entt::entity aEntity) const noexcept;
    //! The character lives again: the pose its body last rested in is dropped (CorpseRestPose).
    void ForgetRestPose(entt::entity aEntity) const noexcept;

protected:
    enum class OwnershipTransferReason : uint8_t
    {
        LeaderAssignment,
        LeaderClaim,
        Mount,
        Relinquish,
        OwnerUnavailable,
        KillingBlow,
        CrimeWitness,
        Hold,
        HoldEnded,
        NearestMember
    };

    void OnUpdate(const UpdateEvent& acEvent) const noexcept;
    void OnCharacterExteriorCellChange(const CharacterExteriorCellChangeEvent& acEvent) const noexcept;
    void OnCharacterInteriorCellChange(const CharacterInteriorCellChangeEvent& acEvent) const noexcept;
    void OnAssignCharacterRequest(const PacketEvent<AssignCharacterRequest>& acMessage) const noexcept;
    void OnOwnershipTransferRequest(const PacketEvent<RequestOwnershipTransfer>& acMessage) const noexcept;
    void OnOwnershipTransferEvent(const OwnershipTransferEvent& acEvent) const noexcept;
    void OnOwnershipClaimRequest(const PacketEvent<RequestOwnershipClaim>& acMessage) const noexcept;
    void OnFollowerStateRequest(const PacketEvent<RequestFollowerState>& acMessage) const noexcept;
    void OnFollowerCommandRequest(const PacketEvent<RequestFollowerCommand>& acMessage) noexcept;
    void OnPlayerLeaveFollowerCommands(const PlayerLeaveEvent& acEvent) noexcept;
    void OnStuckArrowRequest(const PacketEvent<RequestStuckArrow>& acMessage) const noexcept;
    void OnDismemberRequest(const PacketEvent<RequestDismember>& acMessage) const noexcept;
    void OnCharacterRemoveEvent(const CharacterRemoveEvent& acEvent) const noexcept;
    void OnCharacterSpawned(const CharacterSpawnedEvent& acEvent) const noexcept;
    void OnReferencesMoveRequest(const PacketEvent<ClientReferencesMoveRequest>& acMessage) const noexcept;
    void OnActionEventsRequest(const PacketEvent<RequestActionEvents>& acMessage) const noexcept;
    //! The actions a script does not cancel, made the actor's current one and kept for players who come into range later.
    Vector<ActionEvent> KeepActions(entt::entity aEntity, AnimationComponent& aAnimationComponent, const Vector<ActionEvent>& acActions) const noexcept;
    void OnFactionsChanges(const PacketEvent<RequestFactionsChanges>& acMessage) const noexcept;
    void OnMountRequest(const PacketEvent<MountRequest>& acMessage) const noexcept;
    void OnDismountRequest(const PacketEvent<RequestDismount>& acMessage) const noexcept;
    void OnNewPackageRequest(const PacketEvent<NewPackageRequest>& acMessage) const noexcept;
    void OnRequestRespawn(const PacketEvent<RequestRespawn>& acMessage) const noexcept;
    void OnClashRequest(const PacketEvent<ClashRequest>& acMessage) const noexcept;
    void OnArrowNockedRequest(const PacketEvent<ArrowNockedRequest>& acMessage) const noexcept;
    void OnSyncExperienceRequest(const PacketEvent<SyncExperienceRequest>& acMessage) const noexcept;
    void OnDialogueRequest(const PacketEvent<DialogueRequest>& acMessage) const noexcept;
    void OnSubtitleRequest(const PacketEvent<SubtitleRequest>& acMessage) const noexcept;

    void CreateCharacter(const PacketEvent<AssignCharacterRequest>& acMessage) const noexcept;
    //! Answers a game's new temporary actor with the one another game of its party spawned for the same event (TwinSpawn), if any.
    bool PairScriptSpawn(const PacketEvent<AssignCharacterRequest>& acMessage) const noexcept;
    //! Another connected game stands an actor of its own for this spawn.
    bool HasPairedGame(entt::entity aEntity) const noexcept;
    void PopulateAssignmentResponse(entt::entity aEntity, AssignCharacterResponse& aResponse) const noexcept;
    static const char* GetOwnershipTransferReasonName(OwnershipTransferReason aReason) noexcept;
    bool CanClaimOwnership(Player* apPlayer, entt::entity aEntity, uint32_t aExpectedOwnershipEpoch, OwnershipTransferReason aReason, bool aFollower = false) const noexcept;
    bool TransferOwnership(Player* apPlayer, entt::entity aEntity, OwnershipTransferReason aReason, bool aResetInvalidOwners = true) const noexcept;
    void TransferToNextOwner(entt::entity aEntity, OwnershipTransferReason aReason) const noexcept;
    //! Makes the actor this player's follower if nobody else's stands in the way, and gives her to their game, then tells the others if anything changed.
    //! With aGrantAgain a game that has her on the server is granted her once more: it asked because it declined the first grant, with her not yet loaded.
    void MakeFollowerOf(Player* apPlayer, entt::entity aEntity, bool aGrantAgain = false) const noexcept;
    void TakeFollower(Player* apPlayer, entt::entity aEntity) const noexcept;
    //! Gives a follower to her player's game; true when she changed hands.
    //! If the rules for claims refuse, she goes back to aPreviousFollower, as she stays with the game that has her.
    bool GiveFollowerToPlayer(Player* apPlayer, entt::entity aEntity, uint32_t aPreviousFollower, bool aGrantAgain = false) const noexcept;
    //! Sends the character afresh to everybody in range but its owner and its follower's player, who then show it as what it now is.
    void ResendToViewers(entt::entity aEntity) const noexcept;
    //! Takes an actor off an owner who has gone silent or out of range and gives it to a player who can see it.
    void HandOffAbandonedActors() const noexcept;
    //! A party member near an NPC the leader's game runs far from its own player, to take it over; null when none should (OwnerSpread).
    Player* FindMemberToTakeOver(
        entt::entity aEntity, const OwnerComponent& acOwner, const CellIdComponent& acCell, bool aIsDragon, std::chrono::steady_clock::time_point aNow) const noexcept;
    ActorData BuildActorData(const entt::entity acEntity) const noexcept;

    void ProcessFactionsChanges() const noexcept;
    //! Hands the party leader the NPCs its game announced right after an arrival, once the grace period is over.
    void ProcessPendingLeaderAssignments() const noexcept;
    //! Queues a party leader's assignment or claim made within 10 s of its arrival; false when it can go at once.
    bool HoldUntilSettled(Player* apLeader, entt::entity aEntity, uint32_t aOwnershipEpoch, OwnershipTransferReason aReason) const noexcept;
    //! A party member grabbed a living NPC in its ragdoll: its game runs the NPC for the hold (GrabHold).
    //! False when refused.
    bool ClaimToHold(Player* apPlayer, entt::entity aEntity, uint32_t aExpectedOwnershipEpoch) const noexcept;
    //! A hold ends, told by its holder (apHolder) or because it ran too long (null): the NPC goes back, or stays by the usual rules.
    void EndHold(entt::entity aEntity, const Player* apHolder) const noexcept;
    //! Forgets holds that are over and ends those that ran too long.
    void SweepHolds() const noexcept;
    //! Removes an NPC from the views of players its move took it out of range of, and sends it to those it came into range of.
    void UpdateViewsAfterMove(const Player* apOwner, entt::entity aEntity, const CellIdComponent& acBefore, const CellIdComponent& acNow, bool aIsDragon) const noexcept;

private:
    World& m_world;

    // A party leader's assignment held back after their arrival (a load): who, which actor, at which epoch, and when.
    struct PendingLeaderAssignment
    {
        uint32_t PlayerId;
        entt::entity Entity;
        uint32_t OwnershipEpoch;
        std::chrono::steady_clock::time_point ReadyAt;
        OwnershipTransferReason Reason;
    };
    mutable Vector<PendingLeaderAssignment> m_pendingLeaderAssignments;
    // Living NPCs held by a party member's grab, by entity id (GrabHold).
    mutable TiltedPhoques::Map<uint32_t, GrabHold::Hold> m_holds;
    // Holds ended for running too long, by entity id: their holder waits before holding the NPC again.
    mutable TiltedPhoques::Map<uint32_t, GrabHold::Rest> m_holdRests;

    // The witnesses crime reports named lately: whose report, the witness's epoch then, and when.
    mutable CrimeReports::WitnessBook m_crimeWitnesses;

    // The pose each dead NPC's body came to rest in at its owner's, by entity id, for the players sent the body later.
    mutable CorpseRestPose::Store<VRPose> m_restPoses;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_exteriorCellChangeEventConnection;
    entt::scoped_connection m_interiorCellChangeEventConnection;
    entt::scoped_connection m_characterAssignRequestConnection;
    entt::scoped_connection m_transferOwnershipConnection;
    entt::scoped_connection m_ownershipTransferEventConnection;
    entt::scoped_connection m_claimOwnershipConnection;
    entt::scoped_connection m_followerStateConnection;
    entt::scoped_connection m_followerCommandConnection;
    entt::scoped_connection m_followerCommandLeaveConnection;
    //! Words to followers per player, a few a second at most (FollowerCommand::RateLimit).
    TiltedPhoques::Map<uint32_t, FollowerCommand::RateLimit> m_followerCommandRates;
    //! Followers each player reports as its own, a few a second at most (FollowerCommand::ClaimRateLimit).
    mutable TiltedPhoques::Map<uint32_t, FollowerCommand::ClaimRateLimit> m_followerClaimRates;
    entt::scoped_connection m_removeCharacterConnection;
    entt::scoped_connection m_characterSpawnedConnection;
    entt::scoped_connection m_referenceMovementSnapshotConnection;
    entt::scoped_connection m_factionsChangesConnection;
    entt::scoped_connection m_mountConnection;
    entt::scoped_connection m_dismountConnection;
    entt::scoped_connection m_newPackageConnection;
    entt::scoped_connection m_requestRespawnConnection;
    entt::scoped_connection m_clashConnection;
    entt::scoped_connection m_arrowNockedConnection;
    entt::scoped_connection m_syncExperienceConnection;
    entt::scoped_connection m_dialogueConnection;
    entt::scoped_connection m_subtitleConnection;
    entt::scoped_connection m_actionEventsConnection;
    entt::scoped_connection m_stuckArrowConnection;
    entt::scoped_connection m_dismemberConnection;
};
