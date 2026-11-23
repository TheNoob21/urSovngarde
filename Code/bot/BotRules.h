#pragma once

#include <cstdint>

namespace BotRules
{
// The target of the bot's additem ref:<hex> command: an NPC it has seen, alive and in its current ownership, that it owns
// or that follows a player of its party. The server still checks the actor, epoch, sender, range and party.
constexpr bool MaySendInventoryToRef(bool aPlayer, bool aDead, uint32_t aEpoch, bool aOwned, uint32_t aFollowerOf, bool aFollowerInParty) noexcept
{
    return !aPlayer && !aDead && aEpoch != 0 && (aOwned || (aFollowerOf != 0 && aFollowerInParty));
}
} // namespace BotRules
