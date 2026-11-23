#pragma once

#include <cstddef>
#include <cstdint>

//! How this game's copies of other games' characters, and the game's own actors that stand for those characters, are freed and hidden.
//! Pure functions of what the caller read from the game, so they can be tested without it.
namespace CopyLifetime
{
//! What becomes of the actor under a temporary id when the server removes its character.
enum class ServerRemoval : uint8_t
{
    //! A copy this client made, or nothing has that id here: it goes as any removed copy.
    RemoveCopy,
    //! The game's own temporary actor, made by this game's scripts for the same event as another game's: it is the game's to free,
    //! never this client's. The character only left this player's view, so it is remembered as handed away and comes back as itself.
    KeepAsHandedAway,
    //! The game's own actor, its character gone from the server while it stands here: this game runs it now and announces it again.
    AnnounceAgain,
    //! The game's own actor, its character gone, and it is not standing here or is dead at its owner: only the remote mark comes off.
    LeaveToGame,
};

/**
 * What to do with the actor under a temporary id whose character the server removed.
 *
 * Only a copy this client made is deleted: deleting the game's own actor while its scene or AI still runs takes it from this game.
 *
 * @param aActorHere An actor has that id here now.
 * @param aOwnCopy This client made it.
 * @param aDeleted The game has it marked deleted already.
 * @param aGone The character is gone from the server, not only out of this player's view.
 * @param aDeadAtOwner Its owner's game has it dead: announced from here it would come back alive.
 * @param aStandsHere It has its 3D and is switched on.
 */
constexpr ServerRemoval OnServerRemoval(const bool aActorHere, const bool aOwnCopy, const bool aDeleted, const bool aGone, const bool aDeadAtOwner, const bool aStandsHere) noexcept
{
    if (aOwnCopy || !aActorHere)
        return ServerRemoval::RemoveCopy;
    if (!aGone)
        return aDeleted ? ServerRemoval::LeaveToGame : ServerRemoval::KeepAsHandedAway;
    return aStandsHere && !aDeleted && !aDeadAtOwner ? ServerRemoval::AnnounceAgain : ServerRemoval::LeaveToGame;
}

/**
 * Whether an actor is the one a handed-away record was made for.
 *
 * The game gives a freed temporary actor's id to the next one it makes, so the id alone says nothing: the base must match too.
 * A record with no base (the actor was already freed when it was let go) matches nothing; it would take any new actor on that id for
 * the old one, and that actor would never be shared.
 *
 * @param aRecordedBase The base form id the record kept, 0 for none.
 * @param aActorBase The base form id of the actor now on that id, 0 for none.
 */
constexpr bool IsHandedAwayActor(const uint32_t aRecordedBase, const uint32_t aActorBase) noexcept
{
    return aRecordedBase != 0 && aActorBase == aRecordedBase;
}

/**
 * Whether a copy of the owner's creature stands in for the local actor, which is then hidden as a ghost.
 *
 * Never for a temporary actor: the ghost is kept under its id, and the game gives that id to another actor once it frees this one,
 * which would then be hidden. Nor for a body here that is alive at its owner: it stays a body.
 *
 * @param aForeign The owner has another kind of creature for it.
 * @param aTemporary The local actor is temporary.
 * @param aDeadAgainstOwner It is dead here and stays so though its owner has it alive.
 */
constexpr bool StandsInForForeign(const bool aForeign, const bool aTemporary, const bool aDeadAgainstOwner) noexcept
{
    return aForeign && !aTemporary && !aDeadAgainstOwner;
}

//! A removal of a copy asked for while a load is up or just over, done once the load is over.
//! Kept by handle as well as id: the game reuses a freed form id, and the handle tells the copy from a later actor on the same id.
struct LaterRemoval
{
    uint32_t FormId{};
    //! 0 when nothing had that id when the removal was asked for.
    uint32_t Handle{};
};

/**
 * Keep a removal for after the load. None is ever dropped; the same removal asked for twice is kept once.
 *
 * @return Whether it was added (false: the same one waits already).
 */
template <class TList> bool QueueRemoval(TList& aList, const uint32_t aFormId, const uint32_t aHandle) noexcept
{
    for (const LaterRemoval& acWaiting : aList)
    {
        if (acWaiting.FormId == aFormId && acWaiting.Handle == aHandle)
            return false;
    }
    aList.push_back(LaterRemoval{aFormId, aHandle});
    return true;
}

/**
 * Whether the actor a kept removal's handle names now is still the one it was asked for.
 * When it is not (gone, being freed, or the handle never named one), only the records of the copy are cleared: no actor is touched.
 *
 * @param acRemoval The kept removal.
 * @param aFound The handle names an actor now.
 * @param aFoundFormId That actor's form id.
 * @param aFoundDeleted The game has that actor marked deleted.
 */
constexpr bool IsSameActorAfterLoad(const LaterRemoval& acRemoval, const bool aFound, const uint32_t aFoundFormId, const bool aFoundDeleted) noexcept
{
    return acRemoval.Handle != 0 && aFound && !aFoundDeleted && aFoundFormId == acRemoval.FormId;
}

/**
 * A removed copy nothing holds is freed at once, except while a load is up or just over: it then waits like a held copy, switched on, and
 * its wait frees it once the load is over. At the waiting list's limit it is freed at once, as any copy is.
 *
 * @param aSettled No load is up and none was seen for a moment.
 * @param aWaitingCount The copies waiting to be freed now.
 * @param aMaxWaiting The waiting list's limit.
 */
constexpr bool WaitsOutLoad(const bool aSettled, const size_t aWaitingCount, const size_t aMaxWaiting) noexcept
{
    return !aSettled && aWaitingCount < aMaxWaiting;
}

//! What the main thread finds when a hidden reference's queued switch-off comes up.
struct GhostFound
{
    bool Exiting{};
    //! Its handle still names it, and it is not marked deleted.
    bool Actor{};
    //! A copy still stands in for it (or it is still hidden as this player's follower).
    bool StillGhost{};
    bool Disabled{};
    //! It has its 3D and is not loading: disabling one the game is disposing of crashes in its movement code.
    bool Has3D{};
};

//! Whether a queued switch-off of a hidden reference is done. One released before it came up was never switched off, so nothing is
//! done to it; one switched off meanwhile by its save or scripts is left to them.
constexpr bool SwitchOffGhost(const GhostFound& acFound) noexcept
{
    return !acFound.Exiting && acFound.Actor && acFound.StillGhost && !acFound.Disabled && acFound.Has3D;
}

/**
 * Whether this frame's move of a copy waiting for its 3D is logged: it is moved every frame, so only its first move is, and the first
 * one after each load.
 *
 * @param aLoggedBefore A move of it was logged before.
 * @param aScreensThen The loading screens counted when that was.
 * @param aScreensNow The loading screens counted now.
 */
constexpr bool LogsWaitingMove(const bool aLoggedBefore, const uint32_t aScreensThen, const uint32_t aScreensNow) noexcept
{
    return !aLoggedBefore || aScreensThen != aScreensNow;
}
} // namespace CopyLifetime
