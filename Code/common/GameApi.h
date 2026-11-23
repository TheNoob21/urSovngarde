#pragma once

#include <stddef.h>
#include <stdint.h>

#define UR_GAME_API_VERSION 1u

typedef struct UrGameActor
{
    uint32_t Form, Id, Epoch, Owned, Player;
    uint32_t Dead, Mounted, Sitting, Follower, Loaded;
    uint32_t Cell, World;
    float Position[3];
} UrGameActor;

typedef struct UrGameReference
{
    uint32_t Handle, Form, Base, BaseType, Cell, World;
    uint32_t Disabled, Deleted, Aliased;
    float Position[3], Rotation[3], Scale;
} UrGameReference;

typedef struct UrGamePose
{
    uint32_t Bones[19]; // packed unit quaternions in VRPose bone order
    float Position[3], Heading, Hips[3], LeftHand[3], RightHand[3];
    uint32_t Scale; // thousandths; zero for native scale
} UrGamePose;

typedef struct UrGameHooks
{
    // Native hooks may run outside the network update. Copy values and defer network sends.
    // Created: on the thread that spawned the reference, which is the game's thread or the World update (CreateCopy).
    void (*Created)(const UrGameReference* apReference);
    // Activate: on the game's thread, from the activation hook.
    uint32_t (*Activate)(uint32_t aReference, uint32_t aActivator);
    // FrameEnd: on the render thread at the end of the frame, not in the World update.
    void (*FrameEnd)(void);
} UrGameHooks;

// Called synchronously during held-object presentation, from the frame end (render thread). Commit is valid only within the callback.
typedef uint32_t (*UrCommitPose)(void* apContext, const float aPosition[3], const float aRotation[4]);
typedef void (*UrPresentNode)(uint32_t aForm, uint32_t aSide, const void* apRoot, const void* apNode, void* apContext, UrCommitPose aCommit);

typedef struct UrGameProjectile
{
    uint32_t Shooter, Epoch, Owned, Handle, Form;
    uint64_t Projectile, Weapon, Ammo, Spell;
    float Origin[3], Rotation[3];
} UrGameProjectile;

// Runs in the World update, for projectiles launched here and for those a peer reported.
typedef void (*UrProjectileLaunched)(const UrGameProjectile* apLaunch);

typedef struct UrGameBody
{
    float Position[3], Rotation[4]; // world position and x, y, z, w
} UrGameBody;

// Runs at the frame end (render thread).
typedef uint32_t (*UrCapturesRagdoll)(uint32_t aForm, const void* apRoot);

typedef struct UrGameObjectObservation
{
    uint32_t Form, Handle, Base, Cell, Loads;
    uint32_t Adopt; // Adopt a temporary floor item, or follow motion of a placed object.
    float Previous[3];
} UrGameObjectObservation;

// A consumer runs in the World update (the game's update thread). Supply only copied values.
typedef uint32_t (*UrTakeObjects)(UrGameObjectObservation* apOut, uint32_t aCapacity);
// An extension registers a static table once. Module(nullptr) resets the negotiated load order.
// The chat prefix is a single token. Chat receives the remaining text on the game runner.
// The other callbacks run on the thread that asks: the game's thread, or the World update for messages from the server.
typedef struct UrGameFormPolicy
{
    const char* ChatPrefix;
    void (*Chat)(const char* apText, uint32_t aSize);
    void (*Module)(uint32_t aId, const char* apFilename);
    uint32_t (*RejectMagic)(uint32_t aModule, uint32_t aBase);
    uint32_t (*LocalMagic)(uint32_t aLocalForm);
    uint32_t (*AllowSelfEffect)(uint32_t aArmor, uint32_t aSlotMask, uint32_t aSpell, uint32_t aEffect);
    uint32_t (*SharedSwitch)(uint32_t aReference, uint32_t aBase, uint32_t aBaseType);
} UrGameFormPolicy;

typedef struct UrGameReactionHooks
{
    // An unpaired physical action requested on a copy. Copy values before leaving this callback. Runs in the World update.
    void (*Unpaired)(uint32_t aForm, uint32_t aKind, float aMagnitude, float aDirection);
    // Return the existing stock action kind plus one to claim a native graph event on a copy.
    // Runs on the thread that sends the graph event, which may not be the game's main thread.
    uint32_t (*Graph)(const char* apEvent);
} UrGameReactionHooks;

typedef struct UrGameTouch
{
    const void* Bodies[2];
    uint32_t Filters[2];
    uint8_t MotionTypes[2];
    float Point[3], Velocities[2][3];
    uint8_t Held[2];
    float HeldAt[2][3];
} UrGameTouch;
typedef struct UrGameTouchHooks
{
    // Both run at the frame end (render thread).
    // 0 defers to ordinary handling. 1 admits a sample. 2 rejects it. 3 replaces the compared sample.
    uint32_t (*Select)(const UrGameTouch* apPrevious, const UrGameTouch* apCurrent, uint32_t aHasPoint);
    void (*Impact)(uint32_t aOtherForm, uint32_t aSide, float aSpeed, uint64_t aTick, const float aPoint[3]);
} UrGameTouchHooks;

typedef struct UrGameFallbackPose
{
    // Enabled and Capture run at the frame end (render thread); Read runs in the World update.
    uint32_t (*Enabled)(void);
    void (*Capture)(const uint32_t aBones[7]);
    uint32_t (*Read)(uint32_t aId, uint32_t aEpoch, uint64_t aTick, uint32_t aBones[7]);
} UrGameFallbackPose;

// Another plugin's word on followers: who is this player's follower, and how one outside the game's follower quest takes another
// player's Wait or Follow. Registered with RegisterFollowerPolicy. A null callback, or none registered, leaves it to core, as without the add-on.
typedef struct UrGameFollowerPolicy
{
    // 0: core decides (the teammate flag and CurrentFollowerFaction at rank 0 or above). 1: this player's follower. 2: not a follower.
    // Asked for this game's non-temporary actors other than the player, on the thread that asks (mostly the game's main thread), often
    // and for many actors: keep it to a few lookups. Asking core about actors from inside it gets core's own rule for them.
    uint32_t (*Follower)(uint32_t aActorForm, uint32_t aBaseForm, uint32_t aTeammate);
    // Another player's word (1 wait, 2 follow) for this game's follower whom the game's follower quest does not hold.
    // 0: not handled. Otherwise the reply: 1 done, 2 busy (fighting, bleeding out, talking or riding), 4 no longer a follower. Main thread,
    // up to a few seconds after the word came: check she is still loaded and this player's follower.
    uint32_t (*FollowerWord)(uint32_t aActorForm, uint32_t aCommand);
} UrGameFollowerPolicy;

typedef struct UrGameApi
{
    uint32_t Version, Size;
    uint32_t (*Register)(uint32_t aId, const UrGameHooks* apHooks);
    uint32_t (*Endpoint)(char* apText, uint32_t aCapacity);
    uint32_t (*PluginForm)(uint32_t aFormId, char* apName, uint32_t aCapacity, uint32_t* apBase);
    const void* (*Papyrus)(const char* apClass, const char* apName);
    void* (*VirtualMachine)(void);
    void (*VisitActors)(void* apContext, void (*aVisit)(void*, const UrGameActor*));
    uint32_t (*Actor)(uint32_t aServerId, UrGameActor* apActor);
    // Returns unique armor IDs whose slot mask exactly matches. Output is valid when count <= capacity.
    uint32_t (*WornArmor)(uint32_t aActorForm, uint32_t aSlot, uint32_t* apForms, uint32_t aCapacity);
    uint32_t (*EquipCopyArmor)(uint32_t aActorForm, uint32_t aItemForm, uint32_t aSlot, uint32_t aEquip);
    uint32_t (*Reference)(uint32_t aForm, uint32_t aHandle, UrGameReference* apOut);
    // Create a temporary presentation reference. The caller must first validate its recipe and base form.
    uint32_t (*CreateCopy)(const UrGameReference* apState, uint32_t aInitiallyDisabled, uint32_t aBlockActivation);
    uint32_t (*ApplyReference)(const UrGameReference* apState);
    uint32_t (*RemoveCopy)(uint32_t aForm, uint32_t aHandle);
    uint64_t (*SaveGeneration)(void);
    uint32_t (*WithSave)(uint64_t aGeneration, void* apContext, void (*aWork)(void*));
    uint32_t (*NativeReference)(void* apNative, UrGameReference* apOut);
    uint32_t (*RegisterPresentation)(uint32_t aAddon, UrPresentNode aPresent);
    // Capture only in FrameEnd; the engine rig is final for that frame.
    // The pose callback given to RegisterPose runs in the World update.
    uint32_t (*CapturePose)(UrGamePose* apPose);
    uint32_t (*RegisterPose)(uint32_t aId, uint32_t (*aPose)(uint32_t aServerId, uint32_t aEpoch, uint64_t aTick, UrGamePose* apPose));
    uint32_t (*RegisterProjectile)(uint32_t aId, UrProjectileLaunched aObserver);
    void (*ActorSpeech)(uint32_t aActor, const int16_t* apSamples, uint32_t aCount);
    void (*StopSpeech)(uint32_t aActor);
    void (*Subtitle)(uint32_t aActor, const char* apText);
    uint32_t (*MoveReference)(const UrGameReference* apState, uint32_t aAllowDetachedCell);
    uint32_t (*MotionType)(uint32_t aForm, uint32_t aHandle, uint32_t aMotion);
    uint32_t (*ActorVehicle)(uint32_t aForm, uint32_t aHandle, uint32_t aVehicle, uint32_t aIdle);
    uint32_t (*GlobalValue)(uint32_t aFormId, float* apValue, uint32_t aWrite);
    uint32_t (*ClaimMount)(uint32_t aServerId, uint32_t aEpoch);
    void (*Hud)(const char* apText);
    // Reserved runs on the game's thread or in the World update, BeforeDelete on the game's thread; Step runs on the game's thread once a frame, before the main update.
    uint32_t (*RegisterInteraction)(uint32_t aAddon, uint32_t (*aReserved)(uint32_t), uint32_t (*aBeforeDelete)(uint32_t), void (*aStep)(void));
    uint32_t (*PresentRagdoll)(uint32_t aId, uint32_t aEpoch, uint64_t aTick, uint32_t aLayout, uint32_t aTotal, const UrGameBody* apBodies, uint32_t aCount);
    uint32_t (*RegisterRagdoll)(uint32_t aAddon, UrCapturesRagdoll aCaptures);
    uint32_t (*InteractionReserved)(uint32_t aForm);
    void (*VisitCell)(const uint8_t* apTypes, uint32_t aTypeCount, void* apContext, void (*aVisit)(void*, const UrGameReference*));
    uint32_t (*RegisterObjects)(uint32_t aId, UrTakeObjects aTake);
    uint32_t (*RegisterFormPolicy)(uint32_t aAddon, const UrGameFormPolicy* apPolicy);
    uint32_t (*RegisterReactions)(uint32_t aAddon, const UrGameReactionHooks* apHooks);
    uint32_t (*PresentReaction)(uint32_t aId, uint32_t aEpoch, uint32_t aKind, float aMagnitude, float aDirection, uint32_t aObserver);
    uint32_t (*RegisterTouches)(uint32_t aAddon, const UrGameTouchHooks* apHooks);
    uint32_t (*RegisterFallbackPose)(uint32_t aAddon, const UrGameFallbackPose* apHooks);
    uintptr_t (*VisualRoot)(uint32_t aForm);
    // Nonzero while the local player is in dialogue or has a menu open (inventory, map, console, container, barter and the like).
    // Game thread.
    uint32_t (*LocalPlayerInMenu)(void);
    // A melee hit the local player landed, as a physics mod lands it (aHand 0 right, 1 left; aFlags as the hit event's: 1 power,
    // 4 bash). On VR the other games then see the swing even when the game marked none. Any thread; nothing on a flat game.
    void (*PlayerMeleeHit)(uint32_t aHand, uint32_t aFlags);
    // Members from here on were added later: check UR_GAME_API_HAS before calling one, as an older core leaves it out (Size).
    // Registers this add-on's follower policy (UrGameFollowerPolicy); one per add-on. 1 taken, 0 refused.
    uint32_t (*RegisterFollowerPolicy)(uint32_t aAddon, const UrGameFollowerPolicy* apPolicy);
} UrGameApi;

// Whether the core that gave apApi has the member aMember: its Size reaches past it.
#define UR_GAME_API_HAS(apApi, aMember) ((apApi) && (apApi)->Size >= offsetof(UrGameApi, aMember) + sizeof((apApi)->aMember))

#ifdef __cplusplus
extern "C"
#endif
#ifdef UR_GAME_HOST
    __declspec(dllexport)
#endif
    const UrGameApi*
    urSovngarde_GetGameApi(uint32_t aVersion);
