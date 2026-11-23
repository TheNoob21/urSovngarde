#pragma once

// What the Sleep/Wait menu's OK does while online (SleepWaitOk's hook).
// The menu a member opens to follow the leader's fast travel (PartyService::HasFollowOffer) is no wait: its OK follows the leader.
// Before, it ran the game's own wait, which only this game took.
namespace FollowMenuOk
{
enum class Action
{
    kGameOk, // the game's own OK, as offline
    kVote,   // held as this player's vote in the shared sleep and wait
    kFollow, // closes the menu; the leader's fast travel is then followed
};

/**
 * @param aOnline The server has accepted this player.
 * @param aHoldEnabled The main loop presses the agreed OK (SleepWaitOk::EnableHold).
 * @param aHasHours The OK came with a length.
 * @param aFollowAccepted This menu was opened to follow the leader's fast travel.
 */
constexpr Action Decide(bool aOnline, bool aHoldEnabled, bool aHasHours, bool aFollowAccepted) noexcept
{
    if (aOnline && aFollowAccepted)
        return Action::kFollow;
    if (!aOnline || !aHoldEnabled || !aHasHours)
        return Action::kGameOk;
    return Action::kVote;
}
} // namespace FollowMenuOk
