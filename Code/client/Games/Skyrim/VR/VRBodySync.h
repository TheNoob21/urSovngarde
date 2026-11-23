#include <GameApi.h>
#pragma once

#include <Games/Skyrim/CopyBodyRules.h>
#include <Structs/VRPose.h>
#include <Systems/RemotePoseState.h>

struct Actor;
struct PlayerCharacter;

//! Syncs the pose of VR players: spine, head, arms and hands always.
//! Hips, thighs, calves and feet too when full-body tracking is active, as in the upstream capture policy.
//!
//! Local side: reads the root-relative bone rotations of the player's skeleton (driven by VRIK from the headset and controllers).
//! Remote side: once per frame, at the renderer's frame end, the remote skeleton is posed from the latest interpolated pose.
//! Writing from the animation job threads would let the renderer read half-written bones.
namespace VRBodySync
{
uint32_t CaptureSolvedPose(UrGamePose* apOut) noexcept;
//! Renderer frame end (BSGraphics StopTimer). One thread, once per frame.
void OnFrameEnd() noexcept;
//! The start of a frame, on the main thread: weapon bodies found at the last frame end go in the world here.
void AddPendingWeaponBodies() noexcept;
//! How far a body's pelvis stands over its lower foot, and where the pelvis is. False for a body with no such bones.
bool BodyPelvisHeight(Actor* apActor, float& aHeight, glm::vec3& aPelvis) noexcept;

//! The other player's weapon (on their copy FormId, Side 0 left / 1 right) met one of this player's HIGGS bodies.
struct Clash
{
    uint32_t FormId = 0;
    uint8_t Side = 0;
    //! This player's hand that met it: 0 left, 1 right, 2 not known.
    uint8_t OwnSide = 2;
    glm::vec3 Point{};
    //! Game units a second.
    float Speed = 0.f;
    //! Whether its sound was played.
    bool Heard = false;
    //! True when the two started touching (felt, heard and sent).
    //! False when they are still touching (only weighed by the defender's rule).
    bool Start = true;
    //! How far the HIGGS body that met it was from that hand's node.
    float HandDistance = 0.f;
    //! What met it can parry: a weapon or shield in that hand, or something held up with HIGGS.
    //! A bare or spell hand cannot, so such a touch is not felt, heard, sent nor weighed by the defender's rule.
    bool Parries = false;
};
//! The next clash seen at a frame end, felt and heard as it is taken when it is a meeting; false when there is none.
bool TakeClash(Clash& aOut) noexcept;
//! A clash the other player saw on one of this player's weapons: felt on the hand holding it (0 left, 1 right).
void PulseClashHaptics(uint8_t aSide) noexcept;
//! What PlaceCorpse is told besides where the owner has the body.
struct CorpsePlacement
{
    //! Who the copy is shown for now (its interpolation's pose identity and generation). A move is made only under the same.
    CopyBodyRules::CopyAuthority Authority{};
    //! The owner's last pose of the body as this copy's playback has it, or null.
    const VRPose* pOwnerPose = nullptr;
    //! A dead body's resting pose held for when its ragdoll is here (InterpolationComponent::RestPose), or null.
    //! PlaceCorpse clears its HasData once it settles the body into it or has no more use for it.
    VRPose* pRestPose = nullptr;
    //! The owner's bones arrive now: that pose was received within the live timeout.
    bool OwnerBonesArrive = false;
    //! That pose lies between two of the owner's poses, not held past the newest (flat's carry follow).
    bool OwnerBonesMove = false;
    //! A living NPC knocked down here, at rest while its owner's lies elsewhere (CopyBodyRules::PlaceWhereOwnerFell).
    bool KnockedWhereOwnerFell = false;
    //! Which of its falls this is: its one move to where its owner's fell is per fall.
    uint64_t Fall = 0;
};
//! A dead body whose owner has it elsewhere: its ragdoll, which it is drawn from, is moved there.
//! A living NPC knocked down here is moved only right after its owner carried it, or when acHow.KnockedWhereOwnerFell says so.
//! Every rigid body moves by the same offset, when it lies more than 64 units away across the ground, or more than 256 below (at most twice a second).
//! A copy made here whose ragdoll lies deep under the ground and still drops is not moved but switched off (FallStop).
//! Once its owner's bones stop coming, a dead body is laid out once in the owner's last pose instead.
//! Only out of the player's view, never while dying or bleeding out, never under this player's hand or near an open loot menu.
//! On flat the move is made at the start of the next frame (MovePendingRagdolls).
//! The one-shot moves (the settle, the move to where its owner fell) count only once one did what it is for, tried at most
//! CopyBodyRules::kMaxMoveTries times. False when nothing was moved or queued.
bool PlaceCorpse(Actor* apActor, const glm::vec3& acWanted, const CorpsePlacement& acHow = {}) noexcept;
//! The start of a frame, on the main thread: the ragdoll moves PlaceCorpse queued on flat are made here, each only when every gate
//! it was decided under still holds, for the same copy (CopyBodyRules::StillAuthorized); the others are canceled. Carried bodies are set into
//! their owners' here too: on VR only a creature's, from where its owner has each of its bodies (RagdollPoseService).
void MovePendingRagdolls() noexcept;
//! Whether this flat game moves and knocks NPC copies' bodies to match their owners'; false on VR.
bool MovesFlatRagdolls() noexcept;
//! Forgets everything PlaceCorpse and the owner's posing knew of every copy, and cancels every queued move: a new session or a load.
void ForgetAllCorpses() noexcept;
//! This game reported the death of an NPC it runs: its body's bones are sent at least once (CaptureBodyPose),
//! even if it came to rest before it read dead, so the others can lay their copy out in its pose.
void NoteDeathHere(uint32_t aFormId) noexcept;
/**
 * Forget what PlaceCorpse, the fall stop and the owner's posing knew of a copy, and cancel its queued moves, so a new copy under the same id starts afresh.
 *
 * @param aFormId The copy.
 */
void ForgetCorpse(uint32_t aFormId) noexcept;
/**
 * List the names of the nodes in an actor's 3D whose name contains a part, in any case, for checks that cannot look at the screen (the rig).
 *
 * @param apActor The actor.
 * @param acpPart The part of the name to look for.
 * @param aMax The most names listed.
 * @param aCount Receives how many nodes have such a name in all.
 * @return The names, comma separated; empty when there is none.
 */
std::string NodesNamedLike(Actor* apActor, const char* acpPart, size_t aMax, size_t& aCount) noexcept;
/**
 * Log the local player's nodes named like an arrow (VR, first and third person) when their names, parents or hidden flags change.
 * Measurement only: where the game keeps the arrow taken from the quiver and the one on the string.
 * It walks the nodes at most every 200 ms.
 *
 * @param apPlayer The local player.
 * @param aAttackState The player's attack state, written in the line.
 */
void LogLocalArrowNodes(PlayerCharacter* apPlayer, uint32_t aAttackState) noexcept;
//! Measurement: the heading sent for this player against the headset's yaw and the VRIK body's facing (from the thighs).
//! Sampled ten times a second, one `HeadingDiag:` line every 30 s.
void NoteHeading(PlayerCharacter* apPlayer) noexcept;
//! An arrow held in a hand, not on the string: which hand, and where the arrow model sits in the space of its third-person bone.
struct ArrowHold
{
    bool InHand = false;
    //! 0 left, 1 right.
    uint8_t Side = 1;
    glm::vec3 Offset{};
    glm::quat Rotation{1.f, 0.f, 0.f, 0.f};
};
//! The arrow the local player holds in a hand, as the last pose capture found it (at most ten times a second).
ArrowHold LocalArrowHold() noexcept;
//! How far apart the local player's two hands were at that capture, negative when not found.
float LocalArrowHandsApart() noexcept;
//! This copy has an arrow on its bow string, or in a hand (acHold.InHand), or neither (NotifyArrowNocked).
//! The arrow is laid from the string hand to the bow hand, or where the owner holds it, each frame.
//! The game's flat attachment puts it anywhere on a hand posed from VR.
void SetArrowNocked(uint32_t aFormId, bool aNocked, const ArrowHold& acHold = {}) noexcept;
//! Whether this copy has an arrow attached by SetArrowNocked, on the string or in a hand.
bool HasCopyArrow(uint32_t aFormId) noexcept;
//! Puts a copy's weapons (acPose.HasWeapons) and its arrow where the owner holds them, from its hands as they are posed now.
//! For a body posed outside OnFrameEnd, once its hands are final for the frame; apRoot is the copy's 3D root.
//! aFindAgain: the body's tree changed, so the weapon nodes are looked up afresh. Returns how many worlds were written.
uint32_t PlaceHeldWeapons(uint32_t aFormId, void* apRoot, const VRPose& acPose, bool aFindAgain) noexcept;
//! A pass that placed nothing after one that did: how many of those worlds still hold what was placed (0 when the game composed them again).
uint32_t ReleaseHeldWeapons(uint32_t aFormId) noexcept;
//! Where the local player's hand is in the world, left (0) or right (1): the first-person hand, which follows the controller.
bool LocalHandPosition(uint8_t aSide, glm::vec3& aOut) noexcept;
//! Whether this player's HIGGS bodies, or what they hold, touched another player's weapon body at that point in the last half second.
//! A hit there landed on the weapon, not on its owner.
bool IsWeaponTouchAt(const glm::vec3& acPoint) noexcept;
//! Whether one of this player's blades rests on a blade of that copy (the blade stop), now or in the last 300 ms.
//! The hit point must also lie along where the hand holds that blade; such a blade never reached the other player.
//! With no point (zero or not a number), any blade resting on his counts.
bool IsBladeStoppedAt(uint32_t aCopyFormId, const glm::vec3& acPoint) noexcept;
//! The game's sparks of a blade on metal, at a clash's point, for a second; false when none were made.
bool PlayClashSparks(const glm::vec3& acPoint) noexcept;
//! The game's blade-block sound, at a clash's point; false when it could not be played.
bool PlayClashSound(const glm::vec3& acPoint) noexcept;

//! Whether a body plugin (VRIK) fits the player's body to the headset and controllers. Without one the body plays the game's
//! own VR animations, which raise both arms by the head whenever a weapon is drawn, so then its pose is not sent (BodySolver.h).
bool HasBodySolver() noexcept;

//! Reads the player's VR pose in the body's frame; aOutHeading is the heading to send with it, the body's facing when known.
bool CaptureLocalPose(PlayerCharacter* apPlayer, VRPose& aOutPose, float& aOutHeading) noexcept;

//! The last pose CaptureLocalPose read, to send again while the game shows another body; false when it shows the same one.
bool LastLocalPose(PlayerCharacter* apPlayer, VRPose& aOutPose) noexcept;

//! The pose of a dead NPC this machine owns, so the other players see it slump and get dragged as it does here.
//! Upper body, and the legs when the skeleton has them. A body with no humanoid skeleton is sent as its root's position, and its
//! ragdoll's bodies go to RagdollPoseService.
//! Returns false, leaving the pose empty, when the body has not moved since the last send.
bool CaptureBodyPose(Actor* apActor, VRPose& aOutPose) noexcept;
//! Watch a dead body, or a living NPC lying in its ragdoll, this client does not own for signs of being handled here.
//! True once per grab, after a third of a second of motion; the caller then asks for the body so that this side sends it.
//! acOwnerPosition is where the owner's updates put it: motion that follows those is the owner's, not a hand here.
bool IsRemoteBodyGrabbedHere(Actor* apActor, const glm::vec3& acOwnerPosition) noexcept;
//! Weapon touch: a controller pulse when your blade or your hand meets somebody else's weapon.
//!
//! Driven from the game thread once per frame, around the loop that already walks every tracked actor.
//! Call BeginWeaponTouch, then ConsiderForWeaponTouch for each actor, then EndWeaponTouch.
//! Works against NPCs and other players' copies alike, since the copy's weapon is a node like any other.
void BeginWeaponTouch() noexcept;
void ConsiderForWeaponTouch(Actor* apActor) noexcept;
void EndWeaponTouch() noexcept;

//! TEMPORARY: a pose as it arrives for a remote player's copy, for the PoseFlipDiag line (a copy flipping between two poses).
void NoteReceivedPose(uint32_t aFormId, uint64_t aTick, const VRPose& acPose) noexcept;
//! A pose without data clears the actor's pose.
//! Publishes with original receive provenance; live poses expire on a steady-clock deadline.
void SetRemotePoseSample(Actor* apActor, const ::RemotePose::Sample& acSample, double aNowNetworkMs, ::RemotePose::Policy aPolicy) noexcept;
void ClearRemotePose(uint32_t aFormId) noexcept;
void ClearRemotePoseForGeneration(uint32_t aFormId, uint64_t aGeneration) noexcept;

//! Where this player's held spell goes now, in the game's angles.
//! From any thread, each frame; it goes out with the pose while it keeps coming.
void NoteSpellAim(float aZAngle, float aXAngle) noexcept;
//! A new session starts with no held spell aim.
void ResetLocalSpellAim() noexcept;
//! The held spell's aim alone, without claiming any tracked bones or body placement: a flat player's, or a VR player's without
//! a body plugin while a weapon or spell is out (BodySolver.h).
bool CaptureSpellAimOnly(VRPose& aOutPose) noexcept;
//! Where the held spell of another player's copy goes, if its owner sends that. From any thread.
bool CopySpellAim(uint32_t aFormId, float& aZAngle, float& aXAngle) noexcept;
#ifndef SKYRIMVR
//! On the flat game, the one part of a copy's pose it keeps for spells: where its held spell goes.
//! Kept apart from the poses OnFrameEnd draws, so a pose with no bones is never drawn.
void SetRemoteSpellAim(uint32_t aFormId, const VRPose& acPose) noexcept;
#endif

//! A cheap fingerprint of the local transforms of the first nodes below an actor's 3D root; 0 when the actor has no 3D.
//! Equal fingerprints across frames on a moving body mean its animation graph is not advancing.
uint64_t SkeletonMotionFingerprint(Actor* apActor) noexcept;
/**
 * Work out how far a position is from where the player is looking, to tell whether they are looking at it.
 *
 * @param acPosition A position in the game's world.
 * @return The angle in degrees, 0 dead centre; -1 without the headset's node; 180, straight behind, when its numbers are broken.
 */
float HeadsetAngleTo(const NiPoint3& acPosition) noexcept;

/**
 * Find where the player's head is and which way it faces, to place other players' voices around it.
 *
 * @param aPosition Set to where the head is in the game's world.
 * @param aForward Set to which way it faces.
 * @param aRight Set to which way is its right.
 * @return False when the headset cannot be read; the three values are then left as they were.
 */
bool HeadsetPose(glm::vec3& aPosition, glm::vec3& aForward, glm::vec3& aRight) noexcept;

//! Whether the game has moved the VR rig away from the player, as the world map does (to its camera, ~107000 units).
//! Bounded by distance, not by the menu: the rig is not back yet when the map reports itself closed.
bool IsRigParked(const PlayerCharacter* apPlayer) noexcept;

//! TEMPORARY: one line about an actor's 3D for the invisible-copy diagnosis.
//! It lists the root, its children, and the skeleton root's world scale and position.
std::string DescribeBody(Actor* apActor) noexcept;
} // namespace VRBodySync
