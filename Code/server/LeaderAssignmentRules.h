#pragma once

//! When the server asks whether an actor a game registers goes to that game as the party leader's (OwnershipTransferReason::LeaderAssignment).
//! Every game registers the actors it loads, so asking for each one from a game that cannot win logged a refusal each time
//! ("Rejected party leader assignment from player 3 ... the player is not the party leader"). Only the party leader's game can
//! be given a living actor this way, and a dead body changes hands only when it is grabbed: for the rest the answer is no,
//! unasked and unlogged. CanClaimOwnership answers no in exactly these cases, so nothing else changes.
namespace LeaderAssignmentRules
{
inline bool WorthAsking(const bool aInParty, const bool aIsLeader, const bool aIsDead) noexcept
{
    return aInParty && aIsLeader && !aIsDead;
}
} // namespace LeaderAssignmentRules
