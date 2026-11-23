#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

// What the server checks in a game's report before it relays it, with no game in it.
// Each rule refuses only what the server can prove wrong from what it keeps; an honest game is never refused.
namespace ClientTrust
{
enum class Refusal : uint8_t
{
    kNone,
    kNotFinite,
    kUnknownActor,
    kNotOwner,
    kOutOfRange,
    kPvpOff,
    kTooLong,
    kBadPosition,
};

inline const char* RefusalName(const Refusal aRefusal) noexcept
{
    switch (aRefusal)
    {
    case Refusal::kNone: return "none";
    case Refusal::kNotFinite: return "a number that is not finite";
    case Refusal::kUnknownActor: return "an actor the server does not know";
    case Refusal::kNotOwner: return "an actor its sender does not run";
    case Refusal::kOutOfRange: return "an actor out of its sender's range";
    case Refusal::kPvpOff: return "a hit on a player with PvP off";
    case Refusal::kTooLong: return "a text that is too long";
    case Refusal::kBadPosition: return "a position outside the world";
    }
    return "?";
}

// The most one report may add to or take from a health; a larger one is cut to it.
// A killing blow sent for a body that lives on at its owner is 100000 (KillCredit::kDeathWinsBlow).
constexpr float kMaxHealthChange = 1000000.f;

// What a hit or heal on an actor is when its sender reports it.
struct HealthFacts
{
    bool ActorKnown{};
    bool TargetIsPlayer{};   // another player's character
    bool SenderOwnsTarget{}; // a game reports its own actors' health, heals and regeneration too
    bool InRange{};          // the sender has the actor in range
    bool PvpOn{};
};

// aDelta is cut to the largest change when it is above; any other refusal leaves it as sent.
inline Refusal JudgeHealthChange(const HealthFacts& acFacts, float& aDelta) noexcept
{
    if (!std::isfinite(aDelta))
        return Refusal::kNotFinite;
    if (!acFacts.ActorKnown)
        return Refusal::kUnknownActor;
    if (!acFacts.SenderOwnsTarget)
    {
        if (!acFacts.InRange)
            return Refusal::kOutOfRange;
        if (acFacts.TargetIsPlayer)
        {
            // Another game never heals a player's character, and with PvP off it never hurts it.
            if (aDelta >= 0.f)
                return Refusal::kNotOwner;
            if (!acFacts.PvpOn)
                return Refusal::kPvpOff;
        }
    }
    aDelta = std::fmin(std::fmax(aDelta, -kMaxHealthChange), kMaxHealthChange);
    return Refusal::kNone;
}

// A cast, an interruption, a shot or a spell taken off: only the game that runs the caster says them.
inline Refusal JudgeOwnedActor(const bool aActorKnown, const bool aSenderOwns) noexcept
{
    if (!aActorKnown)
        return Refusal::kUnknownActor;
    return aSenderOwns ? Refusal::kNone : Refusal::kNotOwner;
}

// An effect put on an actor: reported by the game that runs the caster, or the one that runs the target
// (a player's game reports what a copy of another game's caster did to it).
inline Refusal JudgeEffect(const bool aTargetKnown, const bool aSenderOwnsTarget, const bool aSenderOwnsCaster, const float aMagnitude) noexcept
{
    if (!std::isfinite(aMagnitude))
        return Refusal::kNotFinite;
    if (!aTargetKnown)
        return Refusal::kUnknownActor;
    return aSenderOwnsTarget || aSenderOwnsCaster ? Refusal::kNone : Refusal::kNotOwner;
}

// The farthest from its shooter a shot may begin: a hand, a bow, a dragon's mouth.
constexpr float kMaxShotOriginDistance = 6000.f;

// aOriginDistance is negative when the server has no position for the shooter yet.
inline Refusal JudgeShot(
    const bool aShooterKnown, const bool aSenderOwnsShooter, const float aOriginDistance, const float aOriginX, const float aOriginY, const float aOriginZ, const float aPower,
    const float aScale) noexcept
{
    if (!std::isfinite(aOriginX) || !std::isfinite(aOriginY) || !std::isfinite(aOriginZ) || !std::isfinite(aPower) || !std::isfinite(aScale))
        return Refusal::kNotFinite;
    if (const Refusal cOwned = JudgeOwnedActor(aShooterKnown, aSenderOwnsShooter); cOwned != Refusal::kNone)
        return cOwned;
    if (aOriginDistance > kMaxShotOriginDistance)
        return Refusal::kOutOfRange;
    return Refusal::kNone;
}

// What an actor says: a voice line's file name or a subtitle's text.
constexpr size_t kMaxSpeechLength = 1024;

// Anyone near an NPC can start its dialogue (its owner's game plays it), so range, not ownership, is asked.
inline Refusal JudgeSpeech(const bool aActorKnown, const bool aSenderOwns, const bool aInRange, const size_t aTextLength) noexcept
{
    if (!aActorKnown)
        return Refusal::kUnknownActor;
    if (!aSenderOwns && !aInRange)
        return Refusal::kOutOfRange;
    return aTextLength > kMaxSpeechLength ? Refusal::kTooLong : Refusal::kNone;
}

// An actor value as it travels.
inline bool IsSaneValue(const float aValue) noexcept
{
    return std::isfinite(aValue);
}

// Where the world ends for the game's own floats; the game's worlds stay far inside it.
constexpr float kMaxCoordinate = 5.0e6f;
// Faster than a fall from a giant's club or a dragon in a dive.
constexpr float kMaxSpeed = 10000.f;
// What a step may exceed its speed by, for jitter, a cell edge and the game's own small moves.
constexpr float kStepSlack = 1000.f;
// Two reports closer in time than this count as this far apart, so two reports in one tick do not make a speed of their own.
constexpr uint64_t kMinStepMs = 50;
// A held jump is a teleport when the next report is still this near it.
constexpr float kConfirmRadius = 256.f;

struct Position
{
    float X{};
    float Y{};
    float Z{};
};

inline float Distance(const Position& acA, const Position& acB) noexcept
{
    const float cX = acA.X - acB.X;
    const float cY = acA.Y - acB.Y;
    const float cZ = acA.Z - acB.Z;
    return std::sqrt(cX * cX + cY * cY + cZ * cZ);
}

inline bool IsInsideWorld(const Position& acPosition) noexcept
{
    return std::isfinite(acPosition.X) && std::isfinite(acPosition.Y) && std::isfinite(acPosition.Z) && std::fabs(acPosition.X) <= kMaxCoordinate &&
           std::fabs(acPosition.Y) <= kMaxCoordinate && std::fabs(acPosition.Z) <= kMaxCoordinate;
}

// A place and a turn that came with a report, both in numbers the game can use.
inline bool IsUsableTransform(const float aX, const float aY, const float aZ, const float aRX, const float aRY, const float aRZ) noexcept
{
    return IsInsideWorld({aX, aY, aZ}) && std::isfinite(aRX) && std::isfinite(aRY) && std::isfinite(aRZ);
}

enum class Step : uint8_t
{
    kTake,   // the position is taken and relayed
    kHold,   // a jump beyond the speeds: not taken yet, and taken as a teleport if the next report stays there
    kRefuse, // not a position
};

// A jump the server holds back until the next report shows whether it was a teleport.
struct HeldJump
{
    bool Held{};
    Position At{};
};

// aFromTick is 0 for an actor with no report yet and after a hand-over: its first position is taken.
// A cell change is a load or a door, and is taken: only a teleport inside one cell is held for its next report.
inline Step JudgeStep(const Position& acFrom, const uint64_t aFromTick, const Position& acTo, const uint64_t aToTick, const bool aCellChanged, HeldJump& aHeld) noexcept
{
    if (!IsInsideWorld(acTo))
        return Step::kRefuse;
    if (aFromTick == 0 || aCellChanged)
    {
        aHeld = HeldJump{};
        return Step::kTake;
    }

    const uint64_t cElapsed = aToTick > aFromTick ? std::max(aToTick - aFromTick, kMinStepMs) : kMinStepMs;
    const float cAllowed = kMaxSpeed * static_cast<float>(cElapsed) / 1000.f + kStepSlack;
    if (Distance(acFrom, acTo) <= cAllowed)
    {
        aHeld = HeldJump{};
        return Step::kTake;
    }

    if (aHeld.Held && Distance(aHeld.At, acTo) <= kConfirmRadius)
    {
        aHeld = HeldJump{};
        return Step::kTake;
    }
    aHeld = HeldJump{true, acTo};
    return Step::kHold;
}

// A correction the server sends a game that was wrong, at most once in this long for each sender.
constexpr std::chrono::seconds kCorrectionEvery{5};

struct CorrectionWindow
{
    std::chrono::steady_clock::time_point LastAt{};

    bool Due(const std::chrono::steady_clock::time_point aNow) noexcept
    {
        if (LastAt.time_since_epoch().count() && aNow - LastAt < kCorrectionEvery)
            return false;
        LastAt = aNow;
        return true;
    }
};
} // namespace ClientTrust
