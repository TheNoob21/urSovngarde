#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

// What a player home shares and what each player keeps, and whose switches of the world are shared, with no game in them.
// A home is a shared house with each player's own storage: its doors, levers, furniture and lights are used together,
// its chests and mannequins stay each player's own unless the server shares homes whole (bSyncPlayerHomes).
// Any player's levers, buttons and switches change the world for the party; the server keeps the last switch of each reference
// (ReferenceSwitches).
namespace SharedHomes
{
// Skyrim.esm: the encounter zone of player homes, and the race of their mannequins (the same ids on SE and VR).
constexpr uint32_t kNoResetZone = 0x0F90B1;
constexpr uint32_t kMannequinRace = 0x10760A;

//! Whether an object of a home is shared: everything outside homes, and in a home all but storage, unless homes are shared whole.
inline bool IsShared(const bool aInHome, const bool aIsStorage, const bool aSyncHomes) noexcept
{
    return !aInHome || aSyncHomes || !aIsStorage;
}

// A member's switch is shared only for a while after that player used a lever or button (an activator), or after its own game reached a quest
// step new to the party (a stage, start or stop the party has not had in this run), so its own quests' switches reach the party too.
constexpr uint64_t kSwitchAfterUseMs = 10000;
// After this game replays another player's use, its script's switches are that player's game's to send, for this long.
constexpr uint64_t kReplayQuietMs = 2000;
// The most of a member's switches the server takes from one message, the newest first; a quest's siege of many is the leader's.
constexpr size_t kMaxMemberSwitches = 32;
// Never, for a time since something that has not happened.
constexpr uint64_t kNever = UINT64_MAX;

//! Whether one of a member's triggers lets its game share a switch: within the window after it, unless a replay of another game's
//! trigger of the same kind came here since, whose game sends what that switched.
inline bool TriggerShares(const uint64_t aSinceOwnMs, const uint64_t aSinceReplayMs) noexcept
{
    return aSinceOwnMs <= kSwitchAfterUseMs && !(aSinceReplayMs < aSinceOwnMs && aSinceReplayMs <= kReplayQuietMs);
}

//! Whether this game shares a switch it made: the leader's always; a member's just after its player used an activator (unless another
//! player's activator use replayed here since), or just after its own game reached a quest step (aSinceOwnStepMs; unless another game's
//! quest step was applied here since, aSinceQuestReplayMs). Each kind is weighed against its own replays.
inline bool SharesSwitch(
    const bool aIsLeader, const uint64_t aSinceUseMs, const uint64_t aSinceReplayMs, const uint64_t aSinceOwnStepMs = kNever, const uint64_t aSinceQuestReplayMs = kNever) noexcept
{
    if (aIsLeader)
        return true;
    return TriggerShares(aSinceUseMs, aSinceReplayMs) || TriggerShares(aSinceOwnStepMs, aSinceQuestReplayMs);
}

//! Whether this game shares no switch now, the leader's either: while its quest catch-up runs (aCatchUpRunning), and for kReplayQuietMs after
//! (aSinceCatchUpEndMs), as caught-up stages' scripts switch a little later. Those are the party's past, which the other games already have;
//! sent again they would take back what a lever or a later stage changed since.
inline bool CatchUpHoldsSwitches(const bool aCatchUpRunning, const uint64_t aSinceCatchUpEndMs) noexcept
{
    return aCatchUpRunning || aSinceCatchUpEndMs <= kReplayQuietMs;
}

//! How many of a player's switches the server takes: none from a player outside a party, all of the leader's,
//! and of another member's the first kMaxMemberSwitches after a use of theirs, or a quest step of theirs new to the party, that the server saw.
inline size_t SwitchesTaken(const bool aInParty, const bool aIsLeader, const bool aUsedRecently, const size_t aCount) noexcept
{
    if (!aInParty)
        return 0;
    if (aIsLeader)
        return aCount;
    return aUsedRecently ? std::min(aCount, kMaxMemberSwitches) : 0;
}

//! Whether an item's Havok motion type holds it still (keyframed 4, fixed 5), as a weapon rack's script holds its weapon.
//! A loose item is dynamic (1 to 3, or 6), and so is one a VR hand just let go of.
inline bool IsHeldStillMotion(const uint8_t aMotionType) noexcept
{
    return aMotionType == 4 || aMotionType == 5;
}

//! Whether an item is kept still where it was left: only a put-down says so; any move hands it back to physics.
inline bool KeptStill(const bool aAtRest, const bool aHeldStill) noexcept
{
    return aAtRest && aHeldStill;
}

} // namespace SharedHomes
