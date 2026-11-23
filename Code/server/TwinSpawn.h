#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <glm/glm.hpp>
#include <TiltedCore/Buffer.hpp>

#include <Structs/GameId.h>

//! A scene or script runs in every game, so each game spawns its own temporary actors for one event (an ambush, a dragon).
//! The server pairs a game's new temporary actor with the one another game already made for the same event,
//! so that the event has one set of actors, run by the game that spawned first.
//! Plain values in and out, so the unit tests check the rule without a server (CharacterService applies it).
namespace TwinSpawn
{
//! How long after one game's spawn another game's spawn of the same kind can still be the same event.
//! Two players reach a scene's trigger a few seconds apart when they travel together.
constexpr uint32_t kWindowSeconds = 120;

//! Farthest apart the two spawns may be made. A script places its actors at the same markers in every game.
constexpr float kReachUnits = 512.f;

//! A game's new temporary actor, as its assignment request has it.
struct Spawn
{
    uint32_t PlayerId{};
    GameId BaseId{};
    GameId LeveledNpcPickId{};
    GameId CellId{};
    GameId WorldSpaceId{};
    glm::vec3 Position{};
};

//! A temporary actor the server already has.
struct Candidate
{
    uint32_t OwnerId{};
    //! Its owner and the new spawn's player are in the same party.
    bool SameParty{};
    GameId BaseId{};
    GameId LeveledNpcPickId{};
    GameId CellId{};
    GameId WorldSpaceId{};
    //! Where it was made, not where it stands now.
    glm::vec3 SpawnPosition{};
    uint32_t AgeSeconds{};
    //! The new spawn's player's game was already given this one for an actor of its own.
    bool PairedWithPlayer{};
};

enum class Miss : uint8_t
{
    kNone,
    kSamePlayer,
    kOtherParty,
    kBase,
    kPick,
    kPlace,
    kTooOld,
    kAlreadyPaired,
    kTooFar,
};

//! Why this candidate is not the new spawn's twin, or kNone when it is one.
inline Miss Check(const Spawn& acSpawn, const Candidate& acCandidate) noexcept
{
    if (acCandidate.OwnerId == acSpawn.PlayerId)
        return Miss::kSamePlayer;
    if (!acCandidate.SameParty)
        return Miss::kOtherParty;
    if (!acSpawn.BaseId || acCandidate.BaseId != acSpawn.BaseId)
        return Miss::kBase;
    // A levelled base rolls in each game; only two known rolls can disagree.
    if (acSpawn.LeveledNpcPickId && acCandidate.LeveledNpcPickId && acCandidate.LeveledNpcPickId != acSpawn.LeveledNpcPickId)
        return Miss::kPick;
    if (acCandidate.CellId != acSpawn.CellId || acCandidate.WorldSpaceId != acSpawn.WorldSpaceId)
        return Miss::kPlace;
    if (acCandidate.AgeSeconds > kWindowSeconds)
        return Miss::kTooOld;
    if (acCandidate.PairedWithPlayer)
        return Miss::kAlreadyPaired;
    if (glm::distance(acCandidate.SpawnPosition, acSpawn.Position) > kReachUnits)
        return Miss::kTooFar;
    return Miss::kNone;
}

//! The candidate the new spawn stands for: the nearest twin, then the oldest; none if no candidate is one.
inline std::optional<std::size_t> FindTwin(const Spawn& acSpawn, const std::vector<Candidate>& acCandidates) noexcept
{
    std::optional<std::size_t> best;
    float bestDistance = 0.f;
    for (std::size_t i = 0; i < acCandidates.size(); ++i)
    {
        const Candidate& cCandidate = acCandidates[i];
        if (Check(acSpawn, cCandidate) != Miss::kNone)
            continue;
        const float cDistance = glm::distance(cCandidate.SpawnPosition, acSpawn.Position);
        if (!best || cDistance < bestDistance || (cDistance == bestDistance && cCandidate.AgeSeconds > acCandidates[*best].AgeSeconds))
        {
            best = i;
            bestDistance = cDistance;
        }
    }
    return best;
}

//! A candidate worth naming in the log when the new spawn has no twin: the same kind made by another game of the party
//! in the same place lately, so a pairing was close. Missed for a reason the log should show.
inline bool IsNearMiss(Miss aMiss) noexcept
{
    return aMiss == Miss::kPick || aMiss == Miss::kTooFar || aMiss == Miss::kAlreadyPaired;
}

//! The reason in words, for the server log.
inline const char* Describe(Miss aMiss) noexcept
{
    switch (aMiss)
    {
    case Miss::kNone: return "paired";
    case Miss::kSamePlayer: return "made by the same game";
    case Miss::kOtherParty: return "another party's";
    case Miss::kBase: return "another kind";
    case Miss::kPick: return "another levelled pick";
    case Miss::kPlace: return "another place";
    case Miss::kTooOld: return "made too long before";
    case Miss::kAlreadyPaired: return "already paired with this game";
    case Miss::kTooFar: return "made too far away";
    }
    return "unknown";
}
} // namespace TwinSpawn
