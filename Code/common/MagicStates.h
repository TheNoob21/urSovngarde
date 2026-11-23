#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include <TiltedCore/Stl.hpp>

#include <Structs/MagicState.h>

// The rules for what a player is seen casting and wearing as magic, with no game in them (MagicStateService).
// A player's own lasting effects (a cloak, Oakflesh, Invisibility) and the held spell they cast reach the other games live,
// as an effect and as a cast. A player who comes into range later, and a copy whose body is made after the cast, missed them.
// So the owner's game also says what its player has now; the server keeps the latest and sends it with the character,
// and each game puts on its copy what is missing and ends what its owner no longer has. The copy's effects only show:
// what they do to others is the caster's game's to decide.
namespace MagicStates
{
// How often the owner looks at its player, and how often it says an unchanged state again so the server's clock stays close.
constexpr uint64_t kReadEveryMs = 250;
constexpr uint64_t kHeartbeatMs = 5000;
// An effect with less left than this is not put on a copy: it would end before it is seen.
constexpr float kShortestLeft = 0.5f;
// A day of real time: no spell lasts longer, and a longer one is refused.
constexpr float kLongestDuration = 86400.f;
// An effect put on the copy this recently may be newer than the state that lacks it (the live effect came first), so it stays.
constexpr float kNewOnCopy = 1.5f;

inline bool IsGoodNumber(const float aValue) noexcept
{
    return std::isfinite(aValue);
}

//! A state from the network, cleaned: effects with no spell, no effect, bad times or a bad magnitude are dropped.
inline MagicState Sanitized(const MagicState& acState) noexcept
{
    MagicState clean;
    for (const auto& effect : acState.Effects)
    {
        if (clean.Effects.size() >= MagicState::kMaxEffects)
            break;
        if (!effect.SpellId || !effect.EffectId || !IsGoodNumber(effect.Magnitude) || !IsGoodNumber(effect.Elapsed) || !IsGoodNumber(effect.Duration))
            continue;
        if (effect.Duration <= 0.f || effect.Duration > kLongestDuration || effect.Elapsed < 0.f || effect.Elapsed >= effect.Duration)
            continue;
        clean.Effects.push_back(effect);
    }
    clean.LeftHand = acState.LeftHand;
    clean.RightHand = acState.RightHand;
    return clean;
}

//! The state as it is aSeconds after it was said: each effect has run that much longer, and those that ran out are gone.
inline MagicState Aged(const MagicState& acState, const float aSeconds) noexcept
{
    MagicState aged;
    aged.LeftHand = acState.LeftHand;
    aged.RightHand = acState.RightHand;
    const float cSeconds = IsGoodNumber(aSeconds) && aSeconds > 0.f ? aSeconds : 0.f;
    for (auto effect : acState.Effects)
    {
        effect.Elapsed += cSeconds;
        if (effect.Elapsed < effect.Duration)
            aged.Effects.push_back(effect);
    }
    return aged;
}

//! The same effects and casts, whatever their times: only a change of these is sent at once.
inline bool SameShape(const MagicState& acA, const MagicState& acB) noexcept
{
    if (acA.Effects.size() != acB.Effects.size() || !(acA.LeftHand == acB.LeftHand) || !(acA.RightHand == acB.RightHand))
        return false;
    for (size_t i = 0; i < acA.Effects.size(); ++i)
        if (acA.Effects[i].SpellId != acB.Effects[i].SpellId || acA.Effects[i].EffectId != acB.Effects[i].EffectId)
            return false;
    return true;
}

//! Whether the owner sends its state now: when it changed, or a heartbeat while it has something.
inline bool ShouldSend(const bool aSentBefore, const bool aSameShape, const bool aEmpty, const uint64_t aSinceSentMs) noexcept
{
    if (!aSentBefore)
        return !aEmpty;
    if (!aSameShape)
        return true;
    return !aEmpty && aSinceSentMs >= kHeartbeatMs;
}

//! An effect on a copy, by this game's spell and effect form ids.
struct Key
{
    uint32_t Spell{};
    uint32_t Effect{};

    bool operator==(const Key& acRhs) const noexcept { return Spell == acRhs.Spell && Effect == acRhs.Effect; }
};

//! One of the copy's effects as this game has it.
struct OnCopy
{
    Key Which{};
    //! How long it has run here.
    float Elapsed{};
    //! A player's own lasting effect of the kind a state carries; only these are ended by a state.
    bool Shared{};
};

struct Plan
{
    //! Indexes into the wanted effects: put these on the copy.
    TiltedPhoques::Vector<size_t> Add{};
    //! Indexes into the copy's effects: end these.
    TiltedPhoques::Vector<size_t> End{};
};

//! What to do to a copy's effects for the owner's state. A wanted effect the copy has is left as it is.
//! aEndMissing is set when a new state came: the copy's shared effects it does not name end, unless just put on.
inline Plan PlanEffects(
    const TiltedPhoques::Vector<Key>& acWanted, const TiltedPhoques::Vector<float>& acWantedLeft, const TiltedPhoques::Vector<OnCopy>& acCopy, const bool aEndMissing) noexcept
{
    Plan plan;
    const auto cOnCopy = [&acCopy](const Key& acKey)
    {
        for (const auto& effect : acCopy)
            if (effect.Which == acKey)
                return true;
        return false;
    };
    const auto cWanted = [&acWanted](const Key& acKey)
    {
        for (const auto& key : acWanted)
            if (key == acKey)
                return true;
        return false;
    };

    for (size_t i = 0; i < acWanted.size(); ++i)
    {
        const float cLeft = i < acWantedLeft.size() ? acWantedLeft[i] : 0.f;
        if (!cOnCopy(acWanted[i]) && cLeft >= kShortestLeft)
            plan.Add.push_back(i);
    }

    if (aEndMissing)
        for (size_t i = 0; i < acCopy.size(); ++i)
            if (acCopy[i].Shared && !cWanted(acCopy[i].Which) && acCopy[i].Elapsed >= kNewOnCopy)
                plan.End.push_back(i);

    return plan;
}

//! What one hand of a copy does to match its owner, from the spell it casts here and the one its owner casts (0 for none).
struct CastStep
{
    bool Interrupt{};
    bool Cast{};
};

inline CastStep PlanCast(const uint64_t aCastingHere, const uint64_t aWanted) noexcept
{
    if (aCastingHere == aWanted)
        return {};
    return {aCastingHere != 0, aWanted != 0};
}

// The game's caster states from starting a cast to holding it ready (MagicCaster::State 1 to 5); 0 is idle, 6 casting, 7 to 9 ending.
constexpr uint32_t kFirstChargeState = 1;
constexpr uint32_t kLastChargeState = 5;

//! Whether a hand shows its owner charging a spell: its caster is charging a fire-and-forget spell that is not sent as a cast,
//! and its owner is alive. A summon counts, so the conjuring hand glows too; only its cast stays with the owner.
inline bool IsCharging(const uint32_t aCasterState, const bool aFireAndForget, const bool aSentAsCast, const bool aDead) noexcept
{
    return aCasterState >= kFirstChargeState && aCasterState <= kLastChargeState && aFireAndForget && !aSentAsCast && !aDead;
}

//! What one hand of a copy does to show its owner's charge glow, from the spell it glows with here and the one its owner charges (0 for none).
//! A copy whose body was made again lost its glow with the old body, so it is played again.
struct ChargeStep
{
    bool Stop{};
    bool Play{};
};

inline ChargeStep PlanCharge(const uint64_t aGlowingHere, const uint64_t aWanted, const bool aBodyChanged) noexcept
{
    if (aGlowingHere == aWanted && !(aBodyChanged && aWanted != 0))
        return {};
    return {aGlowingHere != 0, aWanted != 0};
}

// A copy's glow is played for this long at a time, and played again while the charge lasts, so it ends by itself after it.
constexpr float kGlowSeconds = 1.f;
// Played again this long before it ends. The glows are checked every frame (MagicStateService::RenewDueGlows), not only on a pass
// of the copies, so two frames of a slow game (kSlowFrameMs) are enough for it never to go out in between, and the next glow and
// the last overlap only that long (with 400 ms checked every pass, they overlapped up to 400 ms of every 600 ms).
constexpr uint64_t kSlowFrameMs = 34;
constexpr uint64_t kGlowRenewMs = 2 * kSlowFrameMs;

//! Whether a copy's hand glowing with a charge plays its glow again, with aMsLeft of the last one left.
inline bool RenewsGlow(const uint64_t aGlowingHere, const uint64_t aMsLeft) noexcept
{
    return aGlowingHere != 0 && aMsLeft <= kGlowRenewMs;
}
} // namespace MagicStates
