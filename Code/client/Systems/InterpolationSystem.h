#pragma once

struct World;
struct Actor;

/**
 * @brief Manages interpolation of movement and animations.
 */
struct InterpolationSystem
{
    // aNow: the local network clock in ms (TransportService::GetLastPollTime), which advances the actor's playback clock.
    // aDeadAtOwner: the owner's game has said the actor is dead (RemoteComponent::DeadAtOwner).
    // aLastActionTick: the owner's tick of the last action played on the actor (RemoteAnimationComponent::LastRanAction), 0 for none.
    // aOwnerFlags1: the owner's state word with that action (its life and knock states), kept even when the action was refused here.
    static void Update(Actor* apActor, InterpolationComponent& aInterpolationComponent, double aNow, bool aDeadAtOwner, uint64_t aLastActionTick, uint32_t aOwnerFlags1) noexcept;
    // aArrivalTime: when the update carrying the point arrived (TransportService::GetMessageReceiveTime).
    static void AddPoint(InterpolationComponent& aInterpolationComponent, const InterpolationComponent::TimePoint& acPoint, double aArrivalTime) noexcept;
    // The owner tick this actor is shown at, 0 until its first point arrived.
    static uint64_t GetPlaybackTick(const InterpolationComponent& acInterpolationComponent) noexcept;
    static InterpolationComponent& Setup(World& aWorld, entt::entity aEntity) noexcept;
    // Derive presentation identity/policy from the accepted server lifecycle, never from a movement packet.
    static void BindPoseIdentity(World& aWorld, entt::entity aEntity) noexcept;
    static void ResetPoseSession(World& aWorld) noexcept;
    static void TickPresentation() noexcept;
    static void Clean(World& aWorld, entt::entity aEntity) noexcept;
    // The copy on this form id is gone, or this game runs the actor now: a quiet stop kept for it no longer applies.
    // Left behind, it would start or stop the next copy given that id.
    static void Forget(uint32_t aFormId) noexcept;
    // Every copy's body state (knocked, bleeding out, ragdoll moves and those queued): a new session, or a save loaded.
    static void ForgetCopyBodies() noexcept;
    // An accepted flat NPC fall action arms real bone observations, never native state bits.
    static void NoteReplayedFall(World& aWorld, Actor* apActor, bool aEssentialDown, uint64_t aNativeLifeAtAction, uint64_t aActionTick) noexcept;
    // Its owner's NPC got up in the same life as the fall; a revive by the game or a script still ends the fall.
    static void NoteOwnerGetUp(World& aWorld, Actor* apActor, uint64_t aActionTick) noexcept;
    // One of another player's actions as it plays on that player's copy: whether its owner fell or got up (PlayerKnockMirror.h).
    static void NotePlayerOwnerAction(uint32_t aFormId, const char* acpEventName, uint32_t aOwnerFlags1) noexcept;
    // Another player's "bleedOutStop" reached that player's copy: its owner left its bleedout (CopyBodyRules::PlayerUpAfterKneel).
    static void NotePlayerOwnerStop(uint32_t aFormId, uint64_t aActionTick) noexcept;
    // A jump, fall or landing event sent to a player copy's graph, and whether it took it (JumpSync::Landing).
    static void NoteCopyJumpEvent(uint32_t aFormId, const char* acpEventName, bool aTaken) noexcept;
    // VR: whether the owner's replayed "Ragdoll" plays on this player copy, by the same decision a flat frame knocks it with
    // (PlayerKnockMirror::Mirror::AdmitRagdoll). aHistory: from the history a copy is spawned with. aOwnerFlags1: the owner's
    // state word sent with this Ragdoll.
    static bool AdmitPlayerRagdoll(World& aWorld, Actor* apActor, bool aHistory, uint32_t aOwnerFlags1) noexcept;
    static void NoteOwnerAlive(World& aWorld, Actor* apActor) noexcept;
    static void NoteOwnerHealth(World& aWorld, Actor* apActor, float aHealth, bool aAboveCopyFloor) noexcept;
    // A replayed action's variables keep the speed the copy's legs were given (StrideMatch), not its owner's speed at that action.
    static void HoldStride(Actor* apActor, AnimationVariables& aVariables) noexcept;
    // Not sitting, riding, sleeping or flying.
    static bool OnFoot(const Actor* apActor) noexcept;
    // How fast an actor's legs go while it is on its feet, 0 when it is not.
    static float FootSpeed(Actor* apActor) noexcept;
    // How fast its owner's last two reports have a copy going, in units a second, 0 for one stopped here for going quiet.
    static float ReportedSpeed(uint32_t aFormId, const InterpolationComponent& acInterpolationComponent) noexcept;
};
