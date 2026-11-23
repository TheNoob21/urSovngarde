#pragma once

#include <chrono>
#include <cstdint>

// A dragon's soul for every player of the party, once per dragon (Code/client/DragonSouls.cpp, urSovngardeDragonSoulEffect.psc).
// The game's own absorb (MQKillDragonScript.DeathSequence) waits for this game's player to come near and gives only them the soul.
// Every game therefore keeps its dragons out of it (MQNoDragonAbsorb, which DeathSequence checks first), and the server says when a
// dragon died, once per dragon, to every player of the owner's party. Each game then absorbs it if its player is near, or takes the soul alone.
namespace DragonSoul
{
// A game keeps its dragons out of the game's own absorb only when the server gives souls out (its setting is sent with the server's
// settings), the server has accepted this player on this connection, and this game can take a soul the server gives (the co-op
// menu plugin and its soul spell are loaded). Otherwise the game's own absorb runs, so a dragon is never left with neither.
constexpr bool KeepsDragonsOut(bool aServerGivesSouls, bool aAccepted, bool aCanTakeSouls) noexcept
{
    return aServerGivesSouls && aAccepted && aCanTakeSouls;
}

// FACT MQNoDragonAbsorb, Skyrim.esm.
constexpr uint32_t kNoDragonAbsorbFaction = 0x7A0DF;
// The game's own absorb distance when a dragon sets none (DragonActorScript.OnInit: deathFXrange 1000).
constexpr float kAbsorbRange = 1000.f;
// How long a dragon loaded here waits for this player to come that near, before the soul is given without the absorb.
constexpr auto kAbsorbWait = std::chrono::seconds(30);

// The server: a dragon's death gives its soul out once, the first time any game reports it dead.
constexpr bool ShouldGrant(bool aIsDead, bool aIsDragon, bool aAlreadyGranted) noexcept
{
    return aIsDead && aIsDragon && !aAlreadyGranted;
}

enum class Outcome
{
    kIgnore,     // given already here
    kLeftToGame, // a dragon whose soul the game's own story decides (Solstheim, Apocrypha): its absorb runs as alone
    kNoSoul,     // one the game never absorbs (in MQNoDragonAbsorb by its own record)
    kWaitNear,   // loaded here: absorbed when this player comes near, as alone
    kSoulOnly,   // not loaded here: the soul without the absorb
};

// What a game does with the server's word that a dragon died.
constexpr Outcome OnGrant(bool aGivenHere, bool aLeftToGame, bool aNeverAbsorbed, bool aLoadedHere) noexcept
{
    if (aGivenHere)
        return Outcome::kIgnore;
    if (aLeftToGame)
        return Outcome::kLeftToGame;
    if (aNeverAbsorbed)
        return Outcome::kNoSoul;
    return aLoadedHere ? Outcome::kWaitNear : Outcome::kSoulOnly;
}

// Whether a dragon this game put out of the game's own absorb goes back into it, for a save and at a disconnect.
// Only co-op keeps it out: a save, and the game after a disconnect, must absorb it as alone (a living main-quest dragon, say).
// A body whose soul this player had stays out of it, or the game alone would give that soul a second time.
constexpr bool BackInGameAbsorb(bool aKeptOutHere, bool aDead, bool aSoulHadHere) noexcept
{
    return aKeptOutHere && !(aDead && aSoulHadHere);
}

// A dragon waiting for this player: absorbed now, given without the absorb, or still waiting.
enum class Wait
{
    kAbsorb,
    kSoulOnly,
    kKeepWaiting,
};

constexpr Wait WhileWaiting(bool aStillLoaded, float aDistance, std::chrono::steady_clock::time_point aSince, std::chrono::steady_clock::time_point aNow) noexcept
{
    if (!aStillLoaded)
        return Wait::kSoulOnly;
    if (aDistance <= kAbsorbRange)
        return Wait::kAbsorb;
    return aNow - aSince >= kAbsorbWait ? Wait::kSoulOnly : Wait::kKeepWaiting;
}
} // namespace DragonSoul
