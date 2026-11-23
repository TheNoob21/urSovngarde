#pragma once

//! Whether a reference hidden behind a stand-in comes back alive or as a body when its stand-in goes.
//!
//! A stand-in is a copy of the owner's creature shown over a reference that rolled as another kind here (a hare where the owner has a fox).
//! When that creature died, its copy is a body; the owner's game decides the death, so the reference behind it is dead too.
//! Brought back alive, a killed creature would stand up again where it fell, and this game's save would keep it alive.
//! Another player's follower is not this game's to kill: her reference here is left as it is.
//!
//! A reference to lay down waits until it has its 3D, as a cell the player left keeps it unloaded.
//! It does not wait until its cell may have reset: the creature standing there then may be a new one.
//!
//! It is a pure function of plain values with no game types, so it can be tested (see tests/ghost_death.cpp).
namespace GhostDeathPolicy
{
//! The game's iHoursToRespawnCell, 240 hours: the soonest a cell left alone resets and brings back its creatures.
//! A plugin that lowers it is not read here; with one, a creature that respawned sooner can still be laid down.
constexpr float kCellRespawnDays = 10.f;

//! How long a reference waits to be laid down: a day short of kCellRespawnDays.
//! Its wait is counted from the copy's going, and its cell may have been left a little before that.
constexpr float kMaxWaitDays = kCellRespawnDays - 1.f;

enum class Action
{
    Show,    //!< the reference comes back as it was
    LayDown, //!< the reference comes back as a body, killed once it has its 3D
};

//! What happens to the reference when the copy standing in for it goes.
constexpr Action OnStandInGone(const bool aCopyDead, const bool aFollowerCopy) noexcept
{
    return aCopyDead && !aFollowerCopy ? Action::LayDown : Action::Show;
}

//! Whether the copy that went was a body. The copy's own state is read while the game still has it.
//! Freed, or marked deleted as its cell unloads, it no longer says; the word of the game that ran its character does:
//! the owner's for a copy shown here, this game's own for a copy it ran, as it last reported its death.
constexpr bool CopyWasDead(const bool aCopyGone, const bool aCopyDead, const bool aRunnersWord) noexcept
{
    return aCopyGone ? aRunnersWord : aCopyDead;
}

//! Whether a character this game ran and saw die is handed on as killed here: the grant has it alive, so the server has not taken the death.
//! A protected or essential one refuses a killing blow with no killer at its new owner and lives on there; it is not counted, so its copy is not a body.
constexpr bool KilledBeforeHandOff(const bool aDeadHere, const bool aDeadInGrant, const bool aRefusesBlow) noexcept
{
    return aDeadHere && !aDeadInGrant && !aRefusesBlow;
}

//! Whether a reference waiting to be laid down has waited kMaxWaitDays.
//! Game time set back (the online clock restored after a cancelled sleep) counts as no time passed.
constexpr bool HasExpired(const float aQueuedDaysPassed, const float aDaysPassed) noexcept
{
    return aDaysPassed - aQueuedDaysPassed >= kMaxWaitDays;
}

//! What is known of a reference waiting to be laid down, as of this frame.
struct Waiting
{
    bool InMemory{};         //!< the game has the reference loaded at all
    bool Deleted{};          //!< the game is disposing of it; nothing below is read then
    bool Dead{};             //!< it is a body already
    bool TakenOver{};        //!< hidden again behind a new stand-in, or bound to a character since
    bool Expired{};          //!< see HasExpired
    bool HiddenHere{};       //!< its enable has not landed yet
    bool Disabled{};         //!< the game has it disabled
    bool EnableMayLand{};    //!< queued so recently that a disable may be one still being undone
    bool Unique{};           //!< a unique NPC: a body here would be kept against its owner
    bool LifeFollowsOwner{}; //!< its life follows its owner's game whatever this game's save says
    bool Has3D{};            //!< its 3D is loaded and it is no longer loading
};

enum class Step
{
    Wait,     //!< keep it queued and look again next frame
    Forget,   //!< take it off the queue; nothing to announce
    GiveUp,   //!< take it off the queue: it waited too long, its cell may have reset; announce it as it is if loaded
    Announce, //!< take it off the queue and announce it as it is
    Kill,     //!< kill it, take it off the queue, and announce it as a body
};

//! What to do this frame with a reference waiting to be laid down.
constexpr Step OnWaitingFrame(const Waiting& acWaiting) noexcept
{
    if (!acWaiting.InMemory)
        return acWaiting.Expired ? Step::GiveUp : Step::Wait;
    if (acWaiting.Deleted)
        return Step::Forget;
    if (acWaiting.Dead)
        return Step::Announce;
    if (acWaiting.TakenOver)
        return Step::Forget;
    if (acWaiting.Expired)
        return Step::GiveUp;
    if (acWaiting.HiddenHere)
        return Step::Wait;
    if (acWaiting.Disabled && acWaiting.EnableMayLand)
        return Step::Wait;
    // One this game's own save or scripts keep disabled is left as it is, as is one a body here would be kept against its owner.
    if (acWaiting.Disabled || acWaiting.Unique || acWaiting.LifeFollowsOwner)
        return Step::Announce;
    if (!acWaiting.Has3D)
        return Step::Wait;
    return Step::Kill;
}
} // namespace GhostDeathPolicy
