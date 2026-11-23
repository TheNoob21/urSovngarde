#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>

// The rules for another player's hit making an NPC stagger or recoil (HitReactionService), with no game in them.
// The hitter's game says what the hit did; only the NPC's owner plays it, as the game's own action,
// and the replay of that action shows it to every other player.
namespace HitReactions
{
enum class Kind : uint8_t
{
    kStagger = 0,
    kRecoil = 1,
    kLargeRecoil = 2,
};
constexpr uint8_t kKindCount = 3;

// The game's actions for these, the same form in Skyrim.esm for SE and VR.
constexpr uint32_t kActionStaggerStart = 0x138D2;
constexpr uint32_t kActionRecoil = 0x13AF5;
constexpr uint32_t kActionRecoilLarge = 0x13EC8;

inline std::optional<Kind> KindOfAction(const uint32_t aActionId) noexcept
{
    switch (aActionId)
    {
    case kActionStaggerStart: return Kind::kStagger;
    case kActionRecoil: return Kind::kRecoil;
    case kActionRecoilLarge: return Kind::kLargeRecoil;
    default: return std::nullopt;
    }
}

inline uint32_t ActionOf(const Kind aKind) noexcept
{
    switch (aKind)
    {
    case Kind::kRecoil: return kActionRecoil;
    case Kind::kLargeRecoil: return kActionRecoilLarge;
    default: return kActionStaggerStart;
    }
}

// The game's hit event flags (TESHitEvent).
constexpr uint8_t kPowerAttack = 1;
constexpr uint8_t kSneakAttack = 2;
constexpr uint8_t kBash = 4;
constexpr uint8_t kBlocked = 8;

// How long after a hit on a copy this game's own reaction to it may still come, before the hit's flags decide instead.
constexpr std::chrono::milliseconds kWaitForGame{100};

// When this game itself did not stagger a copy it hit, a power attack or bash still does on its owner.
// Not the game's number for that weapon, which only the owner's game could work out: about a medium, large and largest stagger.
// A plain or blocked hit does nothing, as in the game.
inline std::optional<float> FallbackMagnitude(const uint8_t aFlags) noexcept
{
    if (aFlags & kBlocked)
        return std::nullopt;
    const bool cPower = aFlags & kPowerAttack;
    const bool cBash = aFlags & kBash;
    if (cPower && cBash)
        return 0.75f;
    if (cPower)
        return 0.5f;
    if (cBash)
        return 0.35f;
    return std::nullopt;
}

// Where a blow came from, as the game's staggerDirection has it: a fraction of a turn from where the actor faces, clockwise.
// Skyrim's heading is clockwise from +y, so an actor facing angle z looks along (sin z, cos z).
inline float DirectionFrom(const float aActorX, const float aActorY, const float aHeading, const float aFromX, const float aFromY) noexcept
{
    constexpr float cTurn = 6.28318530718f;
    float angle = std::atan2(aFromX - aActorX, aFromY - aActorY) - aHeading;
    angle = std::fmod(angle, cTurn);
    if (angle < 0.f)
        angle += cTurn;
    const float cDirection = angle / cTurn;
    return cDirection >= 1.f ? 0.f : cDirection;
}

// A reaction as it travels: a known kind, finite numbers, the magnitude within 0 to 1 and the direction within a turn.
// False for one that cannot be played.
inline bool Sanitize(const uint8_t aKind, float& aMagnitude, float& aDirection) noexcept
{
    if (aKind >= kKindCount || !std::isfinite(aMagnitude) || !std::isfinite(aDirection))
        return false;
    aMagnitude = std::fmin(std::fmax(aMagnitude, 0.f), 1.f);
    aDirection = std::fmod(aDirection, 1.f);
    if (aDirection < 0.f)
        aDirection += 1.f;
    if (aDirection >= 1.f)
        aDirection = 0.f;
    return true;
}

// What a game does with another player's reaction for an actor.
enum class Role
{
    kNothing,      // it sent it, and already showed what it saw
    kPlay,         // it runs the actor: it plays it as the game's own action, which the replay then brings to every copy
    kPresetFloats, // it has a copy: the copy will play the owner's action, with this hit's magnitude and direction
};

inline Role RoleOf(const bool aIsSender, const bool aIsOwner) noexcept
{
    if (aIsSender)
        return Role::kNothing;
    return aIsOwner ? Role::kPlay : Role::kPresetFloats;
}

// What the owner's actor is doing when the reaction reaches it.
struct ActorState
{
    bool IsPlayer{};
    bool DeadOrDying{};
    bool BleedingOut{};
    bool KnockedDown{};
    bool Flying{};
    bool SittingOrMounted{};
    bool Swimming{};
    bool InPairedAnimation{}; // bIsSynced: a kill move or another paired animation
    bool NoStagger{};         // bNoStagger
    bool Staggering{};        // IsStaggering, on the owner's own actor
    bool Recoiling{};         // IsRecoiling
    bool Has3D{true};
};

enum class Refusal
{
    kNone,
    kPlayer,
    kDead,
    kBleedingOut,
    kKnockedDown,
    kFlying,
    kSittingOrMounted,
    kSwimming,
    kPairedAnimation,
    kNoStagger,
    kNo3D,
    kAlreadyReacting,
    kTooSoon,
};

inline const char* RefusalName(const Refusal aRefusal) noexcept
{
    switch (aRefusal)
    {
    case Refusal::kNone: return "none";
    case Refusal::kPlayer: return "a player";
    case Refusal::kDead: return "dead";
    case Refusal::kBleedingOut: return "bleeding out";
    case Refusal::kKnockedDown: return "knocked down";
    case Refusal::kFlying: return "flying";
    case Refusal::kSittingOrMounted: return "sitting or mounted";
    case Refusal::kSwimming: return "swimming";
    case Refusal::kPairedAnimation: return "in a paired animation";
    case Refusal::kNoStagger: return "not to be staggered";
    case Refusal::kNo3D: return "no 3D";
    case Refusal::kAlreadyReacting: return "already staggering or recoiling";
    case Refusal::kTooSoon: return "reacted just before";
    }
    return "?";
}

// How long one reaction holds off the next on the same actor, so that quick blows cannot keep it staggered.
constexpr std::chrono::milliseconds CooldownOf(const Kind aKind) noexcept
{
    return aKind == Kind::kStagger ? std::chrono::milliseconds(600) : std::chrono::milliseconds(400);
}

inline Refusal
CanPlay(const ActorState& acState, const Kind aKind, const std::chrono::steady_clock::time_point aNow, const std::chrono::steady_clock::time_point aLastPlayed) noexcept
{
    if (acState.IsPlayer)
        return Refusal::kPlayer;
    if (acState.DeadOrDying)
        return Refusal::kDead;
    if (acState.BleedingOut)
        return Refusal::kBleedingOut;
    if (acState.KnockedDown)
        return Refusal::kKnockedDown;
    if (!acState.Has3D)
        return Refusal::kNo3D;
    if (acState.Flying)
        return Refusal::kFlying;
    if (acState.SittingOrMounted)
        return Refusal::kSittingOrMounted;
    if (acState.Swimming)
        return Refusal::kSwimming;
    if (acState.InPairedAnimation)
        return Refusal::kPairedAnimation;
    if (aKind == Kind::kStagger && acState.NoStagger)
        return Refusal::kNoStagger;
    // A stagger is not started again while one plays, nor a recoil over a stagger or another recoil.
    // Two players' blows, or a blow and a shove, landing together make one reaction, not one cut short by the next.
    if (acState.Staggering || (aKind != Kind::kStagger && acState.Recoiling))
        return Refusal::kAlreadyReacting;
    if (aLastPlayed.time_since_epoch().count() && aNow - aLastPlayed < CooldownOf(aKind))
        return Refusal::kTooSoon;
    return Refusal::kNone;
}

// The server takes this many reactions for one actor from one player in a second, and drops the rest.
constexpr uint32_t kServerPerSecond = 4;

// Whether the server relays a player's reaction for an actor.
// Players are never staggered by another game, the owner's game plays its own actor's reactions itself,
// and only a game with the actor near its player can have hit it.
inline bool ServerTakes(const bool aIsCharacter, const bool aIsPlayer, const bool aHasOwner, const bool aSenderIsOwner, const bool aInRange) noexcept
{
    return aIsCharacter && !aIsPlayer && aHasOwner && !aSenderIsOwner && aInRange;
}

// How many reactions one player sent for one actor in the current second.
struct RateCount
{
    std::chrono::steady_clock::time_point Since{};
    uint32_t Count{};

    // Counts one more, and says whether it is still within the limit.
    bool Take(const std::chrono::steady_clock::time_point aNow) noexcept
    {
        if (aNow - Since >= std::chrono::seconds(1))
        {
            Since = aNow;
            Count = 0;
        }
        return ++Count <= kServerPerSecond;
    }
};
// A stagger the game's hit code starts on an NPC copy straight on its graph, not as an action.
// A copy plays only its owner's reactions, so that one is held back here, and its owner's game decides (HitReactionService).
// The owner's stagger still comes, as an action this game replays, which is let through.
struct RawGraphEvent
{
    bool IsCopy{};           // an NPC another game runs
    bool IsPlayer{};         // another player's character: none of this applies
    bool IsStagger{};        // "staggerStart"
    bool PlayingAction{};    // sent while this game plays an action it performs or replays
    bool ReplayedReaction{}; // the copy replayed its owner's reaction just before, whose graph event can come after the action
};

inline bool HoldsRawStagger(const RawGraphEvent& acEvent) noexcept
{
    return acEvent.IsCopy && !acEvent.IsPlayer && acEvent.IsStagger && !acEvent.PlayingAction && !acEvent.ReplayedReaction;
}

// How long after a copy replays its owner's stagger or recoil a raw stagger on it is still that replay's.
constexpr std::chrono::milliseconds kReplayedReactionWindow{500};

// The copies that replayed a reaction lately, by form id.
struct ReplayedReactions
{
    static constexpr size_t kKept = 64;

    struct Entry
    {
        uint32_t FormId{};
        std::chrono::steady_clock::time_point At{};
    };
    Entry Entries[kKept]{};
    size_t Next{};

    void Note(const uint32_t aFormId, const std::chrono::steady_clock::time_point aNow) noexcept
    {
        for (auto& entry : Entries)
            if (entry.FormId == aFormId)
            {
                entry.At = aNow;
                return;
            }
        Entries[Next] = Entry{aFormId, aNow};
        Next = (Next + 1) % kKept;
    }

    bool Recently(const uint32_t aFormId, const std::chrono::steady_clock::time_point aNow) const noexcept
    {
        for (const auto& entry : Entries)
            if (entry.FormId == aFormId && entry.FormId != 0 && aNow - entry.At <= kReplayedReactionWindow)
                return true;
        return false;
    }
};
} // namespace HitReactions
