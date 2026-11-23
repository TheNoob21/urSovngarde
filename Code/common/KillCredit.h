#pragma once

#include <chrono>
#include <cstdint>

// Who killed an NPC that one game runs and another player struck last.
// The owner's game kills it with no killer; the killer's own game then kills its copy with its own player as the killer,
// so the kill counts there: kill stats, a bounty, and scripts that ask who the killer was.
namespace KillCredit
{
// The blow a game sends to the owner of an NPC it holds dead while the owner has it alive (CharacterService, DeathWins).
// It only brings two games to the same death, so it credits nobody.
constexpr float kDeathWinsBlow = 100000.f;

// Whether a blow another player landed, and that killed here, is that player's kill.
constexpr bool CreditsAttacker(float aDeltaHealth, uint32_t aAttackerPlayerId) noexcept
{
    return aAttackerPlayerId != 0 && aDeltaHealth < 0.f && aDeltaHealth > -kDeathWinsBlow;
}

// The killer the server passes on: never the player whose game reports the death, and only a player still connected.
constexpr uint32_t Relayed(uint32_t aKillerPlayerId, uint32_t aSenderPlayerId, bool aKillerConnected) noexcept
{
    return aKillerPlayerId == aSenderPlayerId || !aKillerConnected ? 0 : aKillerPlayerId;
}

// Whether this game's player is the killer of a death its owner's game reports.
constexpr bool CreditedHere(uint32_t aKillerPlayerId, uint32_t aLocalPlayerId) noexcept
{
    return aKillerPlayerId != 0 && aKillerPlayerId == aLocalPlayerId;
}

// An owner struck on an NPC already dead in its game says the death again, at most this often per NPC.
// Its copy elsewhere may still stand, waiting for a death word that went missing.
constexpr std::chrono::seconds kDeathRepeatEvery{5};

constexpr bool RepeatDue(std::chrono::steady_clock::time_point aLast, std::chrono::steady_clock::time_point aNow) noexcept
{
    return aLast == std::chrono::steady_clock::time_point{} || aNow - aLast >= kDeathRepeatEvery;
}
} // namespace KillCredit
