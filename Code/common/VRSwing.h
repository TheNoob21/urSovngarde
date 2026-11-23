#pragma once

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>

//! A VR player's swings, power attacks and bashes, as the attack actions a flat player's game performs for the same.
//!
//! A VR player swings with the arm, so the game never performs an attack action for it and the other games' copies of
//! that player did not attack: no swing sound, no attack the NPCs react to. The VR game still marks the swing in the
//! player's attack state (swing or bash) and picks the attack's data (power, bash, which hand). From those, the VR
//! game sends the same actions a flat player's would, and every copy plays a real attack.
namespace VRSwing
{
//! Skyrim.esm's attack actions (BGSAction), the same form ids in every game.
inline constexpr uint32_t kActionLeftAttack = 0x13004;
inline constexpr uint32_t kActionRightAttack = 0x13005;
inline constexpr uint32_t kActionRightPowerAttack = 0x13383;
inline constexpr uint32_t kActionLeftPowerAttack = 0x2E2F6;
inline constexpr uint32_t kActionDualAttack = 0x50C96;
inline constexpr uint32_t kActionDualPowerAttack = 0x2E2F7;
inline constexpr uint32_t kActionLeftRelease = 0x13451;

//! BGSAttackData flags (ATKD).
inline constexpr uint32_t kFlagBash = 1u << 1;
inline constexpr uint32_t kFlagPower = 1u << 2;

//! ATTACK_STATE_ENUM values (bits 28-31 of the actor state's first word).
inline constexpr uint32_t kStateSwing = 2;
inline constexpr uint32_t kStateBash = 6;

enum class Kind : uint8_t
{
    None,
    Right,
    Left,
    Dual,
    RightPower,
    LeftPower,
    DualPower,
    Bash,
    PowerBash,
};

inline const char* NameOf(const Kind aKind) noexcept
{
    switch (aKind)
    {
    case Kind::Right: return "swing";
    case Kind::Left: return "left swing";
    case Kind::Dual: return "dual swing";
    case Kind::RightPower: return "power attack";
    case Kind::LeftPower: return "left power attack";
    case Kind::DualPower: return "dual power attack";
    case Kind::Bash: return "bash";
    case Kind::PowerBash: return "power bash";
    default: return "none";
    }
}

//! The bow's and crossbow's attack states (ATTACK_STATE_ENUM 8-15: draw, attached, drawn, releasing, released, next attack,
//! follow-through, fire). Whether the VR game sets them for a crossbow is not known yet; VRSwingService logs them.
inline constexpr uint32_t kStateBowFirst = 8;
inline constexpr uint32_t kStateBowLast = 15;

inline bool IsBowState(const uint32_t aState) noexcept
{
    return aState >= kStateBowFirst && aState <= kStateBowLast;
}

//! Whether a change of the attack state is a bow's or crossbow's step, worth a line in the log.
inline bool IsBowStep(const uint32_t aBefore, const uint32_t aNow) noexcept
{
    return aBefore != aNow && (IsBowState(aBefore) || IsBowState(aNow));
}

inline bool IsSwingState(const uint32_t aState) noexcept
{
    return aState == kStateSwing || aState == kStateBash;
}

//! A new swing or bash: the state became swing or bash from anything else, or went from one to the other.
inline bool Starts(const uint32_t aBefore, const uint32_t aNow) noexcept
{
    return IsSwingState(aNow) && aBefore != aNow;
}

inline bool Contains(const std::string_view acText, const std::string_view acWord) noexcept
{
    if (acWord.empty() || acText.size() < acWord.size())
        return false;
    for (size_t i = 0; i + acWord.size() <= acText.size(); ++i)
    {
        size_t j = 0;
        while (j < acWord.size() && std::tolower(static_cast<unsigned char>(acText[i + j])) == std::tolower(static_cast<unsigned char>(acWord[j])))
            ++j;
        if (j == acWord.size())
            return true;
    }
    return false;
}

//! What a swing that started in aState was. aHasData: the game picked an attack, with aFlags and aEvent (its graph event,
//! e.g. "attackPowerStartForward", "attackStartLeftHand", "bashStart"; a sidestep power attack, "attackPowerStartLeft",
//! is still the right hand's). Without one, a swing is a plain right swing and a
//! bash a plain bash.
inline Kind Classify(const uint32_t aState, const bool aHasData, const uint32_t aFlags, const std::string_view acEvent) noexcept
{
    if (!IsSwingState(aState))
        return Kind::None;
    const bool cPower = aHasData && (aFlags & kFlagPower) != 0;
    if (aState == kStateBash || (aHasData && ((aFlags & kFlagBash) != 0 || Contains(acEvent, "bash"))))
        return cPower ? Kind::PowerBash : Kind::Bash;
    if (aHasData && Contains(acEvent, "dual"))
        return cPower ? Kind::DualPower : Kind::Dual;
    if (aHasData && Contains(acEvent, "lefthand"))
        return cPower ? Kind::LeftPower : Kind::Left;
    return cPower ? Kind::RightPower : Kind::Right;
}

inline bool IsBash(const Kind aKind) noexcept
{
    return aKind == Kind::Bash || aKind == Kind::PowerBash;
}

inline bool IsPower(const Kind aKind) noexcept
{
    return aKind == Kind::RightPower || aKind == Kind::LeftPower || aKind == Kind::DualPower || aKind == Kind::PowerBash;
}

//! The hand an attack action swings: 0 right, 1 left, 2 both; -1 for any other action.
inline int HandOf(const uint32_t aActionId) noexcept
{
    switch (aActionId)
    {
    case kActionRightAttack:
    case kActionRightPowerAttack: return 0;
    case kActionLeftAttack:
    case kActionLeftPowerAttack: return 1;
    case kActionDualAttack:
    case kActionDualPowerAttack: return 2;
    default: return -1;
    }
}

inline int HandOf(const Kind aKind) noexcept
{
    switch (aKind)
    {
    case Kind::Left:
    case Kind::LeftPower: return 1;
    case Kind::Dual:
    case Kind::DualPower: return 2;
    case Kind::None: return -1;
    default: return 0;
    }
}

struct Step
{
    uint32_t ActionId{};
    //! The graph event a flat player's game names the action with.
    std::string_view EventName{};
};

struct Steps
{
    std::array<Step, 2> Items{};
    size_t Count{};
    //! A shield or weapon raised only for this bash, to be lowered when the bash ends (ReleaseStep).
    bool RaisedBlock{};
};

//! The actions a flat player's game performs for that attack, in order, one a frame. A bash is a right attack while
//! blocking: unless the player already blocks, the block is raised first and lowered again when the bash ends.
//! acEvent: the attack's own graph event, when the VR game picked one; it names a plain or power attack's action.
inline Steps StepsFor(const Kind aKind, const bool aBlocking, const std::string_view acEvent = {}) noexcept
{
    Steps steps{};
    const auto add = [&steps](const uint32_t aActionId, const std::string_view acName) noexcept
    {
        steps.Items[steps.Count++] = Step{aActionId, acName};
    };
    const auto named = [&acEvent](const std::string_view acDefault) noexcept
    {
        return acEvent.empty() ? acDefault : acEvent;
    };
    switch (aKind)
    {
    case Kind::Right: add(kActionRightAttack, named("attackStart")); break;
    case Kind::Left: add(kActionLeftAttack, named("attackStartLeftHand")); break;
    case Kind::Dual: add(kActionDualAttack, named("attackStartDualWield")); break;
    case Kind::RightPower: add(kActionRightPowerAttack, named("attackPowerStartInPlace")); break;
    case Kind::LeftPower: add(kActionLeftPowerAttack, named("attackPowerStartLeftHand")); break;
    case Kind::DualPower: add(kActionDualPowerAttack, named("attackPowerStartDualWield")); break;
    case Kind::Bash:
    case Kind::PowerBash:
        if (!aBlocking)
        {
            add(kActionLeftAttack, "blockStart");
            steps.RaisedBlock = true;
        }
        if (aKind == Kind::PowerBash)
            add(kActionRightPowerAttack, "bashPowerStart");
        else
            add(kActionRightAttack, "bashStart");
        break;
    default: break;
    }
    return steps;
}

inline constexpr Step kReleaseStep{kActionLeftRelease, "blockStop"};

//! Whether a swing still needs its actions sent, when the game itself already sent an attack action for that hand
//! around the same time (aSentHand, -1 for none): a plain swing is then shown already; a power attack or bash is not.
inline bool NeedsSending(const Kind aKind, const int aSentHand) noexcept
{
    if (aKind == Kind::None)
        return false;
    if (aSentHand < 0 || IsPower(aKind) || IsBash(aKind))
        return true;
    return aSentHand != HandOf(aKind);
}
//! A block held up: what to send when the player's IsBlocking changes. aShown: a held block's raise went out, sent here or by the
//! game's own action. aBashRaised: a block raised only for a bash is up; it is the bash's own and comes down with it.
enum class BlockChange : uint8_t
{
    None,
    Raise,
    Lower,
};

inline BlockChange HeldBlock(const bool aBlocking, const bool aShown, const bool aBashRaised) noexcept
{
    if (aBlocking && !aShown && !aBashRaised)
        return BlockChange::Raise;
    if (!aBlocking && aShown)
        return BlockChange::Lower;
    return BlockChange::None;
}

inline constexpr Step kRaiseStep{kActionLeftAttack, "blockStart"};

//! A melee hit this game's player landed, as a physics mod lands it with no attack state set: its hit event's flags.
inline constexpr uint32_t kHitPower = 1;
inline constexpr uint32_t kHitBash = 4;
//! A hit shows no swing of its own when an attack for its hand was shown this shortly before it.
inline constexpr uint64_t kHitWindow = 800;

//! The swing a hit was. aHand: 0 right, 1 left.
inline Kind FromHit(const int aHand, const uint32_t aFlags) noexcept
{
    const bool cPower = (aFlags & kHitPower) != 0;
    if (aFlags & kHitBash)
        return cPower ? Kind::PowerBash : Kind::Bash;
    if (aHand == 1)
        return cPower ? Kind::LeftPower : Kind::Left;
    return cPower ? Kind::RightPower : Kind::Right;
}

//! Whether a hit at aNow still needs its swing shown: no attack for its hand, or for both hands, was shown within kHitWindow
//! before it. acShownAt: by hand (0 right, 1 left, 2 both), when an attack was last shown; 0 for never.
inline bool HitNeedsSwing(const Kind aKind, const std::array<uint64_t, 3>& acShownAt, const uint64_t aNow) noexcept
{
    const int cHand = HandOf(aKind);
    if (cHand < 0)
        return false;
    for (const int cShown : {cHand, 2})
    {
        const uint64_t cAt = acShownAt[static_cast<size_t>(cShown)];
        if (cAt != 0 && aNow >= cAt && aNow - cAt < kHitWindow)
            return false;
    }
    return true;
}

//! Whether a swing seen, or a bash step still to send, may still be sent: the player is in the session and out of a
//! loading screen, and no loading screen was queued since it was seen (aSeenLoads), even one that came and went between
//! two updates. Otherwise it belongs to a world that is gone and is dropped.
constexpr bool KeepsPending(const bool aInSession, const uint32_t aSeenLoads, const uint32_t aLoads) noexcept
{
    return aInSession && aSeenLoads == aLoads;
}
} // namespace VRSwing
