#pragma once

#include <cstdint>

#include <Games/LifeStates.h>

//! When an NPC copy's body is moved, settled or stood up here, against its owner's word.
//!
//! Each game drops a body with its own ragdoll, and a copy that goes down in its own bleedout finishes no bleedout by itself.
//! These say what this game may do about it. They are pure functions of plain values with no game types, so they can be
//! tested (see tests/copy_body_rules.cpp).
namespace CopyBodyRules
{
//! ACTOR_LIFE_STATE, bits 21-24 of flags1 (LifeStates names them): ActorState::LifeStateOf on a plain value, kept constexpr for the rules.
constexpr uint32_t LifeState(const uint32_t aFlags1) noexcept
{
    return (aFlags1 >> 21) & 0xF;
}

//! KNOCK_STATE_ENUM, bits 25-27 of flags1: 0 normal, 5 at rest with the get-up queued, 6 getting up.
constexpr uint32_t KnockState(const uint32_t aFlags1) noexcept
{
    return (aFlags1 >> 25) & 0x7;
}

//! Armed only by an accepted native fall action on this NPC copy. Missing bones never prove a get-up.
struct ObservedFall
{
    bool Down{};
    uint32_t LowSamples{};
    uint32_t HighSamples{};
};

constexpr bool MayObserveFall(const bool aAccepted, const bool aRemoteNpc, const bool aDeadHere, const bool aDeadAtOwner, const bool aSameLifetime) noexcept
{
    return aAccepted && aRemoteNpc && !aDeadHere && !aDeadAtOwner && aSameLifetime;
}

//! A refused action may still be accepted by the native graph, as bleedOutStop already is.
//! Only the authenticated Ragdoll event of this same live NPC life may take that path.
constexpr bool
MayRetryRagdoll(const bool aActionTaken, const bool aRagdollEvent, const bool aRemoteNpc, const bool aDeadHere, const bool aDeadAtOwner, const bool aSameLifetime) noexcept
{
    return !aActionTaken && aRagdollEvent && MayObserveFall(true, aRemoteNpc, aDeadHere, aDeadAtOwner, aSameLifetime);
}

//! Whether an NPC copy that refused its owner's "Ragdoll", as an action and as a graph event, is knocked down by the game itself
//! (AIProcess::KnockExplosion with no push, as PushActorAway knocks it): a kneeling copy's graph refuses the fall its owner's took.
//! Its own knock word then runs its own get-up, and a copy at rest can be put where its owner fell.
constexpr bool KnockAfterRefusedRagdoll(
    const bool aMovesOn, const bool aGraphTookIt, const bool aRagdollEvent, const bool aRemoteNpc, const bool aDeadHere, const bool aDeadAtOwner, const bool aSameLifetime,
    const uint32_t aFlags1) noexcept
{
    return aMovesOn && !aGraphTookIt && MayRetryRagdoll(false, aRagdollEvent, aRemoteNpc, aDeadHere, aDeadAtOwner, aSameLifetime) && KnockState(aFlags1) == 0;
}

//! Whether an NPC copy whose graph took its owner's "Ragdoll" while it knelt in its essential fall is also knocked down by the game itself.
//! The graph throws it flat but leaves it in its bleedout with no knock word, so nothing here took it for knocked: it was never put where
//! its owner's NPC fell, and its get-up was never followed (live-copy-knockdown-place, live-copy-knockdown).
constexpr bool KnockAfterKneelingRagdoll(
    const bool aMovesOn, const bool aActionTaken, const bool aRagdollEvent, const bool aRemoteNpc, const bool aDeadHere, const bool aDeadAtOwner, const bool aSameLifetime,
    const uint32_t aFlags1) noexcept
{
    const uint32_t cLife = LifeState(aFlags1);
    return aMovesOn && aActionTaken && aRagdollEvent && MayObserveFall(true, aRemoteNpc, aDeadHere, aDeadAtOwner, aSameLifetime) &&
           (cLife == LifeStates::kEssentialDown || cLife == LifeStates::kBleedout) && KnockState(aFlags1) == 0;
}

//! How long an essential fall is held down once its owner's push was replayed, while its flat ragdoll is not yet seen.
constexpr uint32_t kKnockProofMs = 3000;

//! Whether an observed essential fall is held down whatever its pelvis reads: its owner's push was replayed on it, and its tumble lifts
//! the pelvis above its feet for a moment, which read as standing ended the fall before the flat ragdoll was seen.
constexpr bool HoldEssentialFall(const bool aEssentialDown, const bool aKnockRequested, const uint32_t aMsSinceKnock) noexcept
{
    return aEssentialDown && aKnockRequested && aMsSinceKnock < kKnockProofMs;
}

//! Whether a copy that refused its owner's "bleedOutStart" as an action gets it as a graph event instead.
//! The action needs the game's own bleedout, and a copy is kept above 0 health (its owner's game decides), so an NPC copy lay in its
//! own fall while its owner's NPC knelt, and a downed player's copy stood. An NPC copy only where flat ragdolls are moved (aMovesOn);
//! a player's copy on every screen, as its graph kneel is ended by its owner's stop or PlayerKneelEnds. Never a dead or dying copy,
//! and never once its owner's word has it dead.
constexpr bool SendRefusedBleedOutStart(const bool aMovesOn, const bool aPlayerCopy, const bool aDeadOrDyingHere, const bool aDeadAtOwner) noexcept
{
    return (aMovesOn || aPlayerCopy) && !aDeadOrDyingHere && !aDeadAtOwner;
}

//! A graph can lie in its ragdoll while the native word stays normal. Native knock states retain their own get-up path.
constexpr bool ObservedWithoutNativeKnock(const bool aObservedDown, const uint32_t aFlags1) noexcept
{
    return aObservedDown && KnockState(aFlags1) == 0;
}

//! An action carries the owner's word from BEFORE it ran. The fall's own word cannot prove its later recovery.
constexpr bool WordAfterObservedFall(const uint64_t aFallTick, const uint64_t aLastActionTick) noexcept
{
    return aFallTick != 0 && aLastActionTick > aFallTick;
}

//! Recovery evidence belongs to one observed fall/life. The fall's own pre-action word is never recovery evidence.
struct OwnerRecoveryEvidence
{
    uint64_t GetUpTick{};
    bool AliveAfterFall{};
    bool HealthyAfterAlive{};

    void Alive() noexcept
    {
        AliveAfterFall = true;
        HealthyAfterAlive = false;
    }
    void Health(const bool aHealthy) noexcept { HealthyAfterAlive = AliveAfterFall && aHealthy; }

    bool Allows(const uint64_t aFallTick, const uint64_t aLastActionTick) const noexcept
    {
        return WordAfterObservedFall(aFallTick, aLastActionTick) && ((GetUpTick > aFallTick && aLastActionTick >= GetUpTick) || (AliveAfterFall && HealthyAfterAlive));
    }
};

inline bool ObserveFall(ObservedFall& aFall, const bool aReadable, const float aHeight, const bool aEssentialDown) noexcept
{
    if (!aReadable)
        return aFall.Down;
    const float cLow = aEssentialDown ? 40.f : 25.f;
    aFall.LowSamples = aHeight < cLow ? aFall.LowSamples + (aFall.LowSamples < 2) : 0;
    aFall.HighSamples = aHeight > 40.f ? aFall.HighSamples + (aFall.HighSamples < 2) : 0;
    if (aFall.LowSamples >= 2)
        aFall.Down = true;
    if (aFall.HighSamples >= 2)
        aFall.Down = false;
    return aFall.Down;
}

//! An action cannot erase this game's death or recoverable bleedout before its local recovery has run.
//! The owner's state is still read separately by OnBleedingOut; keeping this state does not keep the owner down.
constexpr bool KeepLifeStateHere(const bool aPlayerCopy, const uint32_t aFlags1) noexcept
{
    const uint32_t cLife = LifeState(aFlags1);
    return aPlayerCopy || cLife == LifeStates::kDying || cLife == LifeStates::kDead || cLife == LifeStates::kEssentialDown || cLife == LifeStates::kBleedout;
}

//! The ragdoll has come to rest and the get-up is queued.
constexpr uint32_t kKnockQueued = 5;

//! Whether the owner's state word has its NPC on its feet: alive or restrained, and not thrown down.
constexpr bool OwnerStands(const uint32_t aOwnerFlags1) noexcept
{
    const uint32_t cLife = LifeState(aOwnerFlags1);
    return (cLife == LifeStates::kAlive || cLife == LifeStates::kRestrained) && KnockState(aOwnerFlags1) == 0;
}

//! How long the owner's word must have its NPC up while the copy bleeds out here before the copy is taken out.
//! A down and an up close together on the owner's side (a hit that drops it again) are let play out first.
constexpr uint32_t kTakeOutAfterMs = 2000;

//! A player's copy kneeling in its graph after its owner went down. A player's copy never goes down here, so neither the game nor
//! OnBleedingOut ends that kneel: its owner's replayed "bleedOutStop" does, and should that be lost, this does.
//! A player whose reported place is this far from where it was when the copy knelt is up (revived, or respawned elsewhere);
//! a downed one does not move. Measured on the owner's reports, not on the copy, which may still be catching up.
constexpr float kKneelMovedUnits = 128.f;

//! Whether its owner's word from after the kneel has them up: their "bleedOutStop" reached the copy (aStopAfterKneel; its own state
//! word is from before it, still down), or a later action's state word has them on their feet.
constexpr bool PlayerUpAfterKneel(const bool aWordAfterKneel, const bool aStopAfterKneel, const uint32_t aOwnerFlags1) noexcept
{
    return aStopAfterKneel || (aWordAfterKneel && OwnerStands(aOwnerFlags1));
}

//! Whether a player's copy kneeling in its graph is stood up: first on its owner's word, once that has had them up for
//! kTakeOutAfterMs (aOwnerUpForMs, see PlayerUpAfterKneel); should the word never come, once its owner has moved kKneelMovedUnits.
constexpr bool PlayerKneelEnds(const bool aOwnerUp, const uint32_t aOwnerUpForMs, const float aOwnerMovedUnits) noexcept
{
    return (aOwnerUp && aOwnerUpForMs >= kTakeOutAfterMs) || aOwnerMovedUnits >= kKneelMovedUnits;
}

//! An NPC copy and its owner's last word, as of this frame.
struct BleedingOut
{
    bool PlayerCopy{};       //!< another player's copy: players have their own downed system
    bool BleedingOutHere{};  //!< the game has it in its own bleedout here
    bool DeadAtOwner{};      //!< its owner's game has said it died
    bool OwnerWordKnown{};   //!< an action of its owner's has arrived, so OwnerFlags1 is a real word
    uint32_t OwnerFlags1{};  //!< the owner's state word with that action
    uint32_t OwnerUpForMs{}; //!< how long this copy has bled out here while that word had its NPC up
    bool TakenOutLately{};   //!< taken out in the last few seconds already
};

enum class Step
{
    Leave,   //!< nothing to do
    Wait,    //!< its owner is up: look again next frame
    TakeOut, //!< take it out of its bleedout and stand it up
};

//! What to do with an NPC copy that may be lying in its own bleedout while its owner's NPC is up.
//! Never for a copy whose owner says dead: a body must not stand up.
constexpr Step OnBleedingOut(const BleedingOut& acCopy) noexcept
{
    if (acCopy.PlayerCopy || !acCopy.BleedingOutHere || acCopy.DeadAtOwner || !acCopy.OwnerWordKnown || !OwnerStands(acCopy.OwnerFlags1))
        return Step::Leave;
    // Taken out once, it is left to the get-up repairs that now reach it (KnockDown), rather than taken out again each frame.
    if (acCopy.TakenOutLately)
        return Step::Leave;
    return acCopy.OwnerUpForMs >= kTakeOutAfterMs ? Step::TakeOut : Step::Wait;
}

//! Whether a copy that refused its owner's "bleedOutStop", as an action and as a graph event, is put back to standing
//! with "IdleForceDefaultState". A player's copy always is; an NPC copy when its owner's word has it up and it is not dead here.
constexpr bool ForceStandAfterRefusedStop(const bool aPlayerCopy, const bool aDeadOrDyingHere, const uint32_t aOwnerFlags1) noexcept
{
    return aPlayerCopy || (!aDeadOrDyingHere && OwnerStands(aOwnerFlags1));
}

//! The game's ActionBleedoutStop (Skyrim.esm), which Actor::SetLifeState performs when an actor's bleedout ends.
constexpr uint32_t kActionBleedoutStop = 0x13ECA;

//! What a copy's owner's last word says of its NPC, as InterpolationSystem last read it: no word yet, down, or up.
enum class OwnerWord : uint8_t
{
    Unknown,
    Down,
    Up,
};

//! A word that is not proven to come after the copy's observed fall is not known; a copy its owner says is dead is down whatever the word.
constexpr OwnerWord OwnerWordOf(const bool aWordKnown, const uint32_t aOwnerFlags1, const bool aDeadAtOwner = false) noexcept
{
    return aDeadAtOwner ? OwnerWord::Down : !aWordKnown ? OwnerWord::Unknown : OwnerStands(aOwnerFlags1) ? OwnerWord::Up : OwnerWord::Down;
}

//! Whether the life state written here is down or dead: dying, dead, essential down or bleedout.
constexpr bool DownOrDead(const uint32_t aFlags1) noexcept
{
    const uint32_t cLife = LifeState(aFlags1);
    return cLife == LifeStates::kDying || cLife == LifeStates::kDead || cLife == LifeStates::kEssentialDown || cLife == LifeStates::kBleedout;
}

//! The game ends a bleedout with Actor::SetLifeState: it writes the actor alive, then performs ActionBleedoutStop on it.
//! A copy plays only its owner's actions (HookPerformAction), so that stop was dropped: its life said alive while its graph knelt
//! for good, with nothing left to stand it up (live-copy-essential-up). The game's own stop now plays on an NPC copy whose life it
//! has already written out of its fall, while its owner's word has its NPC up.
constexpr bool LetsEngineStopThrough(const bool aNpcCopy, const bool aStopAction, const uint32_t aFlags1, const OwnerWord aOwner) noexcept
{
    return aNpcCopy && aStopAction && !DownOrDead(aFlags1) && aOwner == OwnerWord::Up;
}

//! A copy whose graph kneels while its life says alive: it took its owner's "bleedOutStart" as a graph event only, or the game's
//! stop of its own bleedout was held back above while its owner's NPC is still down. Remembered, so it gets up once its owner's does.
constexpr bool KneelsInGraphOnly(const bool aNpcCopy, const bool aGraphKneels, const uint32_t aFlags1) noexcept
{
    return aNpcCopy && aGraphKneels && !DownOrDead(aFlags1);
}

//! Whether its owner's replayed "bleedOutStop" ends a copy's own bleedout here the game's way (Actor::SetLifeState alive, whose stop
//! then plays) rather than as an action forced while its own life is kept down, which the game refuses.
constexpr bool RecoversOnOwnersStop(const bool aNpcCopy, const uint32_t aOwnFlags1, const uint32_t aOwnerFlags1, const bool aDeadAtOwner) noexcept
{
    // The stop is itself its owner's get-up: its owner's knock state may still read down in that word, its life may not.
    const uint32_t cLife = LifeState(aOwnFlags1);
    const uint32_t cOwnerLife = LifeState(aOwnerFlags1);
    return aNpcCopy && (cLife == LifeStates::kEssentialDown || cLife == LifeStates::kBleedout) && (cOwnerLife == LifeStates::kAlive || cOwnerLife == LifeStates::kRestrained) &&
           !aDeadAtOwner;
}

//! Whether its owner's replayed "bleedOutStop" belongs to a fall before the one the copy kneels in now. A copy's replayed history is
//! queued after its owner's live actions (AnimationSystem::AddActionsForReplay), so the stop of a first fall can arrive during a
//! second, and would stand the copy up while its owner's NPC is down. A stop with no tick, or a copy with no fall yet, is never old.
constexpr bool StopBeforeFall(const bool aNpcCopy, const uint64_t aStopTick, const uint64_t aFallTick) noexcept
{
    return aNpcCopy && aStopTick != 0 && aFallTick != 0 && aStopTick < aFallTick;
}

//! How a copy taken out of its fall (OnBleedingOut) is stood up.
enum class Recovery
{
    Events,       //!< the graph events "bleedOutStop" and "IdleForceDefaultState" (nothing better)
    SetLifeState, //!< the game's own end of a bleedout: Actor::SetLifeState alive, which performs the stop
    PerformStop,  //!< life already alive, the graph kneeling: the stop the game would perform
};

constexpr Recovery RecoverHow(const uint32_t aFlags1, const bool aKneelsInGraph) noexcept
{
    const uint32_t cLife = LifeState(aFlags1);
    if (cLife == LifeStates::kDying || cLife == LifeStates::kDead)
        return Recovery::Events;
    if (cLife == LifeStates::kEssentialDown || cLife == LifeStates::kBleedout)
        return Recovery::SetLifeState;
    return aKneelsInGraph ? Recovery::PerformStop : Recovery::Events;
}

//! A pelvis this far above its lower foot (game units) is standing, not kneeling (a kneel reads 26 to 36).
constexpr float kStandingPelvis = 50.f;
//! A graph kneel is looked at only this long after it was seen, so the kneel's own way down is not taken for standing.
constexpr uint32_t kKneelSettleMs = 1500;
//! A graph kneel is forgotten after this long whatever happens: nothing then stands the copy up against its owner's word.
constexpr uint32_t kKneelForgetMs = 60000;

//! Whether a copy kneeling in its graph only has stood since: two standing readings in a row once it had time to kneel, or too old.
inline bool KneelEnded(uint32_t& aHighSamples, const uint32_t aMsSinceSeen, const bool aReadable, const float aHeight) noexcept
{
    if (aMsSinceSeen >= kKneelForgetMs)
        return true;
    if (aMsSinceSeen < kKneelSettleMs || !aReadable)
        return false;
    aHighSamples = aHeight > kStandingPelvis ? aHighSamples + (aHighSamples < 2) : 0;
    return aHighSamples >= 2;
}

//! A body lies elsewhere than its owner's once it is this far away across the ground, as for the reference.
constexpr float kApartUnits = 64.f;

//! Whether the owner's NPC is still down: no word of its owner's has come since the copy went down here, or the last has it down.
constexpr bool OwnerStillDown(const bool aWordSinceDown, const uint32_t aOwnerFlags1) noexcept
{
    return !aWordSinceDown || !OwnerStands(aOwnerFlags1);
}

//! A living NPC copy knocked down here, as of this frame.
struct Knocked
{
    uint32_t KnockStateHere{}; //!< its own knock state
    bool OwnerStillDown{};     //!< see OwnerStillDown
    float ApartUnits{};        //!< how far its owner's NPC lies, across the ground
};

//! Whether a knocked copy's ragdoll may be put where its owner's NPC fell: once it has come to rest, while its owner's is still down.
//! The owner's push is not replayed, so the copy collapses where it stands; its get-up then starts there.
//! Once per fall: that is the mover's one-shot (OneShot), which counts only a move that was made.
constexpr bool PlaceWhereOwnerFell(const Knocked& acCopy) noexcept
{
    return acCopy.KnockStateHere >= 1 && acCopy.KnockStateHere <= kKnockQueued && acCopy.OwnerStillDown && acCopy.ApartUnits > kApartUnits;
}

//! A knocked ragdoll slower than this (game units a second, its root body) has come to rest.
constexpr float kRestSpeed = 10.f;

//! Whether a knocked copy's ragdoll has come to rest: its get-up queued, or its root body all but still.
//! A copy knocked while its owner's word had it down may wait in its fall until that word changes, never queuing its get-up.
constexpr bool KnockedAtRest(const uint32_t aKnockState, const float aRootSpeed) noexcept
{
    return aKnockState == kKnockQueued || (aKnockState >= 1 && aKnockState < kKnockQueued && aRootSpeed < kRestSpeed);
}

//! Whether an NPC copy's fall replayed from its owner is the start of an essential NPC's bleedout rather than a fall:
//! the copy is essential and at the health floor its owner's 0 left it at, and its owner's word before the fall had it alive.
//! Its owner's NPC kneels there, so the copy is sent "bleedOutStart" instead, and lies only when its owner's is thrown over.
constexpr bool KneelsInsteadOfFalling(
    const bool aMovesOn, const bool aFallEvent, const bool aRemoteNpc, const bool aEssential, const bool aAtHealthFloor, const bool aDeadOrDyingHere, const bool aDeadAtOwner,
    const uint32_t aOwnerFlags1) noexcept
{
    return aMovesOn && aFallEvent && aRemoteNpc && aEssential && aAtHealthFloor && !aDeadOrDyingHere && !aDeadAtOwner && LifeState(aOwnerFlags1) == LifeStates::kAlive &&
           KnockState(aOwnerFlags1) == 0;
}

//! Whether an NPC copy is at the health floor its owner's 0 left it at (KneelsInsteadOfFalling).
//! Its own health alone does not say so for long: kept at the floor here, it heals in the seconds before its owner's fall is replayed.
//! Its owner's last reported health, when one came (NaN when none did), says it too.
constexpr bool AtOwnersHealthFloor(const float aHealthHere, const float aOwnerHealth, const float aFloor) noexcept
{
    return aHealthHere <= aFloor || (aOwnerHealth == aOwnerHealth && aOwnerHealth <= aFloor);
}

//! Whether the owner's arriving bones keep a copy's ragdoll where it lies: they do wherever they pose it.
//! On flat an NPC's bones are never applied (VRBodySync::ApplyRemotePoses), and its owner sends them for as long as its own NPC
//! lies in its ragdoll, with its get-up right after; a knocked copy waiting for them to stop was never put where its owner's NPC fell.
constexpr bool OwnerBonesHoldBody(const bool aBonesArrive, const bool aBonesPoseNpcsHere, const bool aKnockedWhereOwnerFell) noexcept
{
    return aBonesArrive && (aBonesPoseNpcsHere || !aKnockedWhereOwnerFell);
}

//! A copy's body, as of the moment its ragdoll would be moved.
struct BodyHere
{
    bool Dead{};                  //!< dead here
    bool Dying{};                 //!< playing its death here
    bool BleedingOut{};           //!< in its own bleedout here
    bool KnockedWhereOwnerFell{}; //!< a living knocked copy to put where its owner fell (PlaceWhereOwnerFell)
    bool CarriedLately{};         //!< drawn from its owner's bones in the last seconds (someone carried it over there)
    bool OwnerBonesArrive{};      //!< its owner's bones arrive now
    bool HeldHere{};              //!< this player holds it (the grab key, or a hand in VR)
    bool LootOpenNear{};          //!< a container or loot menu is open with the body close to the player
};

//! Whether a copy's ragdoll may be moved at all. Where the player looks is the caller's (it waits until neither end is seen).
//! Never a dying or bleeding-out body, never while its owner's bones arrive (they place it), never under this player's hand or loot menu.
//! A living body only when it is knocked to be put where its owner fell, or was carried over there just now.
constexpr bool MayMoveRagdoll(const BodyHere& acBody) noexcept
{
    if (acBody.Dying || acBody.BleedingOut || acBody.OwnerBonesArrive || acBody.HeldHere || acBody.LootOpenNear)
        return false;
    return acBody.Dead || acBody.KnockedWhereOwnerFell || acBody.CarriedLately;
}

//! Who a copy is shown for: the pose session, its owner (ownership epoch), its server and form ids, and its interpolation's generation.
//! A move decided under one of these is made only under the same: a new owner, a new session or a copy made again cancels it.
struct CopyAuthority
{
    uint64_t Session{};
    uint64_t Owner{};
    uint64_t Entity{};
    uint64_t Generation{};

    constexpr bool operator==(const CopyAuthority& acOther) const noexcept
    {
        return Session == acOther.Session && Owner == acOther.Owner && Entity == acOther.Entity && Generation == acOther.Generation;
    }
    constexpr bool operator!=(const CopyAuthority& acOther) const noexcept { return !(*this == acOther); }
    //! Bound to a copy at all: a session, an entity and a generation.
    constexpr bool Valid() const noexcept { return Session != 0 && Entity != 0 && Generation != 0; }
};

//! What a ragdoll move is for.
enum class MoveKind : uint8_t
{
    Place,      //!< every body by one offset, to where the owner's body lies
    Settle,     //!< each body into the owner's last pose (a dead body, once its owner's bones stopped)
    KnockPlace, //!< a living knocked copy put where its owner's NPC fell
};

//! How a move ended.
enum class MoveOutcome : uint8_t
{
    Canceled,   //!< not made: its authority or a gate failed when it was to run
    NotWritten, //!< no body was written (no bodies, no world, no lock, no wrapper)
    Partial,    //!< some bodies written, not all
    Translated, //!< every body moved by one offset (a Settle whose pose could not be read falls back to this)
    Posed,      //!< every body laid out in the owner's pose
};

//! The outcome of a move that ran: how many of its bodies were written, of how many, and whether into the owner's pose.
constexpr MoveOutcome OutcomeOf(const uint32_t aWritten, const uint32_t aBodies, const bool aPosed) noexcept
{
    if (aWritten == 0 || aBodies == 0)
        return MoveOutcome::NotWritten;
    if (aWritten < aBodies)
        return MoveOutcome::Partial;
    return aPosed ? MoveOutcome::Posed : MoveOutcome::Translated;
}

//! Whether the body jumped on screen: some body was written.
constexpr bool BodyMoved(const MoveOutcome aOutcome) noexcept
{
    return aOutcome == MoveOutcome::Partial || aOutcome == MoveOutcome::Translated || aOutcome == MoveOutcome::Posed;
}

//! Whether a move did what it is for. A Settle only when every body is in the owner's pose; a translation alone is not that.
constexpr bool Succeeded(const MoveKind aKind, const MoveOutcome aOutcome) noexcept
{
    if (aKind == MoveKind::Settle)
        return aOutcome == MoveOutcome::Posed;
    return aOutcome == MoveOutcome::Translated || aOutcome == MoveOutcome::Posed;
}

//! How many times a one-shot move is tried before it is given up: a transient grab, view or missing body does not spend the only
//! correction, and a body that can never be moved is not tried for ever. Each try waits at least the mover's half second.
constexpr uint8_t kMaxMoveTries = 3;

//! A one-shot move (Settle per owner stream, KnockPlace per fall): whether it waits to run, has succeeded, and how often it failed.
struct OneShot
{
    uint8_t Failures{};
    bool Pending{};
    bool Done{};
};

//! Whether a one-shot may be queued or made now: not done, not waiting to run, not given up.
constexpr bool MayStart(const OneShot& acShot) noexcept
{
    return !acShot.Done && !acShot.Pending && acShot.Failures < kMaxMoveTries;
}

//! Whether it was given up: tried kMaxMoveTries times without success.
constexpr bool GaveUp(const OneShot& acShot) noexcept
{
    return !acShot.Done && acShot.Failures >= kMaxMoveTries;
}

//! Queued to run at the next frame's start (flat): not done yet.
constexpr OneShot OnQueued(OneShot aShot) noexcept
{
    aShot.Pending = true;
    return aShot;
}

//! It ran, or was canceled: done only when it succeeded; any other end is one failure.
constexpr OneShot OnFinished(OneShot aShot, const MoveKind aKind, const MoveOutcome aOutcome) noexcept
{
    aShot.Pending = false;
    if (aShot.Done)
        return aShot;
    if (Succeeded(aKind, aOutcome))
        aShot.Done = true;
    else if (aShot.Failures < kMaxMoveTries)
        ++aShot.Failures;
    return aShot;
}

//! Whether a dead copy settles into its owner's last pose now: once its owner's bones have stopped coming, while its one-shot may start.
constexpr bool SettleIntoOwnerPose(const bool aDead, const bool aHasOwnerPose, const OneShot& acShot) noexcept
{
    return aDead && aHasOwnerPose && MayStart(acShot);
}

//! What becomes of a dead copy's resting pose that came while the copy could not lie in it: the one sent with its spawn to a player
//! who sees the body later, or its owner's last bones sent before the copy's ragdoll was there.
enum class HeldRestPose : uint8_t
{
    Keep,   //!< kept until the copy's ragdoll is here
    Settle, //!< handed to the settle as its owner's last pose (SettleIntoOwnerPose), once, then forgotten
    Forget, //!< forgotten: its owner's bones arrive and bring their own last pose, or the copy already lies in this one
};

//! aRagdollHere: the copy is dead here and its ragdoll is there, with no move of it waiting to run.
constexpr HeldRestPose OnHeldRestPose(const bool aOwnerBonesArrive, const bool aRagdollHere, const bool aLiesInIt) noexcept
{
    if (aOwnerBonesArrive || aLiesInIt)
        return HeldRestPose::Forget;
    return aRagdollHere ? HeldRestPose::Settle : HeldRestPose::Keep;
}

//! How long a body's 3D root and its bones must keep still before its owner stops sending them (CaptureBodyPose).
constexpr uint32_t kBodyRestMs = 2000;
//! The longest a dead body's bones are still sent once its root keeps still: a ragdoll that twitches for good is not sent for ever.
constexpr uint32_t kCorpseRestCapMs = 6000;
//! A bone that turned less than this since the last time one moved has not moved: a resting ragdoll still twitches a little.
constexpr float kBoneRestDegrees = 2.f;

//! Whether some bone of a body moved, from the most any of them turned since the last time one did (degrees).
constexpr bool BonesMoved(const float aMostDegrees) noexcept
{
    return aMostDegrees != aMostDegrees || aMostDegrees >= kBoneRestDegrees; // NaN counts as moved
}

//! Whether the owner still sends a body's bones (CaptureBodyPose). They stopped 2 s after its 3D root came to rest, but a ragdoll's
//! limbs often settle after its root: the copy was then laid out in a pose from before the owner's body came to rest, and lay otherwise
//! (live-copy-death-pose: legs, a hand). A dead body's bones are sent until they have kept still for 2 s too,
//! for at most 6 s after its root came to rest. A living body is unchanged: an animation moves its bones while no one touches it.
constexpr bool KeepsSendingBones(const bool aDead, const uint32_t aMsSinceRootMoved, const uint32_t aMsSinceBonesMoved) noexcept
{
    if (aMsSinceRootMoved < kBodyRestMs)
        return true;
    return aDead && aMsSinceBonesMoved < kBodyRestMs && aMsSinceRootMoved < kCorpseRestCapMs;
}

//! The height a copy's ragdoll used to be laid out above where its owner has the body, to fall into place from (game units).
//! Where its owner has a dead body is the owner's 3D root, which follows its ragdoll's root, not the ground (AnimationSystem sends it):
//! the copy was dropped 20 units into its owner's pose, and the fall bent it out of it.
constexpr float kSettleDropUnits = 20.f;
//! How far above where its owner's bodies lay the settle lays the pose, at least.
constexpr float kSettleClearanceUnits = 2.f;
//! The most the copy's own lowest body may lie from the pose's lowest: more means the ground here is not where the copy lies (fallen through, far off).
constexpr float kSettleGroundShiftUnits = 40.f;
//! The farthest a settle may carry the body and still take the ground from where it lies now.
constexpr float kSettleGroundReachUnits = 128.f;
//! A copy's ragdoll slower than this lies on the ground.
constexpr float kRestingUnitsPerSecond = 20.f;

//! What a settle knows of the ground under the copy, as it is about to be laid out in its owner's pose.
struct SettleGround
{
    float LowestHere{};   //!< the lowest of the copy's ragdoll bodies as it lies now (z, game units)
    float LowestPosed{};  //!< the lowest of them in the owner's pose, laid where its owner has the body with no lift
    float Apart{};        //!< how far the settle carries it across the ground
    float FastestSpeed{}; //!< the fastest of its bodies now (units a second)
};

//! How much the pose is raised above where its owner has the body. Laid where the owner's bodies lay, it rests as laid and does not fall
//! into its pose: 2 units up. It is raised further only so far as its lowest body would be below the copy's own lowest one now (the ground
//! here), and never more than the old 20-unit drop. Never lowered: under where the owner's lay could be in the ground. The old drop when
//! the ground here is not known (carried far, still moving, the two far apart, or not numbers).
constexpr float SettleLift(const SettleGround& acGround) noexcept
{
    // Written out for NaN: any reading that is not a number leaves the old drop.
    if (acGround.Apart != acGround.Apart || acGround.Apart > kSettleGroundReachUnits || acGround.FastestSpeed != acGround.FastestSpeed ||
        acGround.FastestSpeed > kRestingUnitsPerSecond)
        return kSettleDropUnits;
    const float cShift = acGround.LowestHere - acGround.LowestPosed;
    if (cShift != cShift || cShift > kSettleGroundShiftUnits || cShift < -kSettleGroundShiftUnits)
        return kSettleDropUnits;
    const float cLift = (cShift > 0.f ? cShift : 0.f) + kSettleClearanceUnits;
    return cLift < kSettleDropUnits ? cLift : kSettleDropUnits;
}

//! Where a settling dead body's targets come from. A pose read from its owner is used when there is one;
//! the offset from the copy's own bodies is for a pose that could not be read.
enum class TargetSource : uint8_t
{
    OwnerPoseSettled, //!< the owner's pose, lifted by SettleLift
    CurrentBodies,    //!< the copy's own bodies' rotations, moved by one offset (no pose, or it could not be read)
};

constexpr TargetSource TargetSourceOf(const bool aPosed) noexcept
{
    return aPosed ? TargetSource::OwnerPoseSettled : TargetSource::CurrentBodies;
}

//! A correction the player can see is blended: the body is moved a part of the way each
//! kBlendStepMs, at most kBlendUnitsPerStep a step, so that it slides into place instead of jumping. A settle into the owner's pose
//! blends over kMinBlendSteps however near it lies. Out of view, a move is still made at once.
constexpr uint32_t kBlendStepMs = 33;
constexpr float kBlendUnitsPerStep = 4.f;
constexpr uint32_t kMinBlendSteps = 15;
//! A far correction slides faster rather than for longer than this.
constexpr uint32_t kMaxBlendSteps = 60;

//! How many steps a blend over aApart units takes.
constexpr uint32_t BlendSteps(const float aApart) noexcept
{
    if (!(aApart > 0.f))
        return kMinBlendSteps;
    const float cSteps = aApart / kBlendUnitsPerStep;
    if (cSteps >= static_cast<float>(kMaxBlendSteps))
        return kMaxBlendSteps;
    const uint32_t cWhole = static_cast<uint32_t>(cSteps);
    const uint32_t cUp = static_cast<float>(cWhole) < cSteps ? cWhole + 1 : cWhole;
    return cUp < kMinBlendSteps ? kMinBlendSteps : cUp;
}

//! The part of what is left that step aStep (from 0) of aSteps covers: each body moves on a straight line at an even pace, and the
//! last step lands it exactly.
constexpr float BlendFraction(const uint32_t aStep, const uint32_t aSteps) noexcept
{
    if (aStep + 1 >= aSteps)
        return 1.f;
    return 1.f / static_cast<float>(aSteps - aStep);
}

//! A queued move, looked at again just before it runs.
struct QueuedMoveNow
{
    MoveKind Kind{};
    bool AuthorityKnown{}; //!< the copy is still known to the mover (not forgotten since)
    bool SameAuthority{};  //!< the copy's authority now is the one the move was decided under
    bool AuthorityFresh{}; //!< the copy's interpolation still runs (it said so lately)
    bool StillRemote{};    //!< the actor is still another game's copy here
    bool SameCell{};       //!< the copy and the player are in the cells they were in when it was decided
    bool Loading{};        //!< a load is on (the client's update may run on another thread)
    bool Dead{};
    bool Dying{};
    bool BleedingOut{};
    bool KnockRagdoll{};    //!< living, in its knock ragdoll (not getting up)
    bool OwnerBonesSince{}; //!< its owner's bones arrived since it was decided
    bool InView{};          //!< the player sees either end of the move
    bool HeldHere{};
    bool LootOpenNear{};
    bool Blended{}; //!< a step of a blend (BlendSteps), which may run in view
};

//! Whether a queued move still runs. Every gate it was decided under must still hold, under the same authority.
constexpr bool StillAuthorized(const QueuedMoveNow& acMove) noexcept
{
    if (!acMove.AuthorityKnown || !acMove.SameAuthority || !acMove.AuthorityFresh || !acMove.StillRemote || !acMove.SameCell || acMove.Loading)
        return false;
    if (acMove.Dying || acMove.BleedingOut || acMove.OwnerBonesSince || (acMove.InView && !acMove.Blended) || acMove.HeldHere || acMove.LootOpenNear)
        return false;
    // A body's moves need it dead; a knocked copy's needs it still lying, alive.
    return acMove.Kind == MoveKind::KnockPlace ? acMove.KnockRagdoll && !acMove.Dead : acMove.Dead;
}
} // namespace CopyBodyRules
