#pragma once

// Which placed reference a player takes into their inventory is told to the server, which keeps it taken for every game,
// also for a player who comes later or loads an older save: plain values, so the rule is unit-tested.

namespace PlacedTakes
{
// aTemporary: a dropped or spawned reference, which only this game has; DroppedItemService shares those itself.
// aQuestItem: a quest object stays for each player to take their own, as their quest needs one too.
// aReplayed: this game's copy of another player took it (activation sync); that player's game tells it.
[[nodiscard]] constexpr bool ShouldShare(bool aTemporary, bool aQuestItem, bool aReplayed) noexcept
{
    return !aTemporary && !aQuestItem && !aReplayed;
}
} // namespace PlacedTakes
