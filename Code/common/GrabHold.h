#pragma once

#include <cstdint>

// The rules for grabbing a living NPC that another game runs, with no game in them.
// Only the game that runs an NPC (its owner) sends how its body moves, so an NPC lying in its ragdoll that a player grabs
// with the game's grab key stays where it lies on every other screen unless the grabber's game runs it. For the hold, the server hands it
// to the grabber's game, party leader or not, and when the hold ends it goes back to the game that ran it, or stays by the usual rules.
namespace GrabHold
{
// The longest a hold keeps an NPC from the game that ran it: then the server ends the hold itself.
constexpr double kMaxHoldSeconds = 120.0;
// The grabber's game ends its hold once the NPC it runs for the hold has been up again (out of its ragdoll) this long.
constexpr double kUpForSeconds = 1.0;
// A claim to hold that is not granted in this long is forgotten.
constexpr double kGrantWithinSeconds = 5.0;
// After a hold that ran too long is ended, its holder may not hold the same NPC again for this long, so it does not bounce.
constexpr double kRestSeconds = 10.0;

// What the server knows when a player asks for a living NPC to hold it.
struct ClaimToHold
{
    bool SwitchOn{};
    bool Alive{};
    bool IsPlayer{};
    bool IsMount{};
    // No form id on the server: a temporary actor, which only its own game can take on.
    bool Temporary{};
    bool HasOwner{};
    bool OwnedBySender{};
    // The NPC's game's player and the asking player are in the same party.
    bool OwnersInSameParty{};
    bool InRange{};
    bool EpochMatches{};
    // Another connected player's follower, which stays with her player's game.
    bool OthersFollower{};
    // A dragon in flight, whose flight is not handed over.
    bool FlyingDragon{};
    // Another player's hold has it now.
    bool HeldByOther{};
    // This player's last hold of it ran too long, a moment ago (IsResting).
    bool Resting{};
};

// A party member may hold a living NPC another party member's game runs, as the party leader may claim it.
constexpr bool MayClaimToHold(const ClaimToHold& acClaim) noexcept
{
    return acClaim.SwitchOn && acClaim.Alive && !acClaim.IsPlayer && !acClaim.IsMount && !acClaim.Temporary && acClaim.HasOwner && !acClaim.OwnedBySender &&
           acClaim.OwnersInSameParty && acClaim.InRange && acClaim.EpochMatches && !acClaim.OthersFollower && !acClaim.FlyingDragon && !acClaim.HeldByOther && !acClaim.Resting;
}

// A hold the server granted (player ids, 0 for none).
struct Hold
{
    uint32_t Holder{};
    uint32_t FormerOwner{};
    // The ownership epoch the hand-over gave the holder.
    uint32_t Epoch{};
    // The holder leads the party, whose game runs living NPCs anyway: it keeps the NPC when the hold ends.
    bool Keep{};
    double Since{};
};

// The hold still has the NPC: its holder runs it under the epoch the hold gave.
// A hand-over since (the holder left, the NPC was handed off) ends it without a word.
constexpr bool IsLive(const Hold& acHold, const uint32_t aOwner, const uint32_t aEpoch) noexcept
{
    return acHold.Holder != 0 && aOwner == acHold.Holder && aEpoch == acHold.Epoch;
}

// While a hold is live, nobody else claims the NPC, the party leader's game included; a killing blow still may.
constexpr bool BlocksClaim(const bool aLive, const bool aFromHolder, const bool aKillingBlow) noexcept
{
    return aLive && !aFromHolder && !aKillingBlow;
}

enum class End : uint8_t
{
    // Not the live hold's holder: nothing changes.
    Ignore,
    // The holder's game keeps the NPC, by the usual rules.
    Keep,
    // The NPC goes back to the game that ran it.
    Return,
};

// When the hold ends, from its holder or because it ran too long.
// aFormerOwnerFits: that player is still connected, in the holder's party, and has the NPC in range.
// A body stays with whoever grabbed it, as any body does.
constexpr End OnEnd(const Hold& acHold, const bool aLive, const bool aFromHolder, const bool aDead, const bool aFormerOwnerFits) noexcept
{
    if (!aLive || !aFromHolder)
        return End::Ignore;
    if (acHold.Keep || aDead || !aFormerOwnerFits || acHold.FormerOwner == 0)
        return End::Keep;
    return End::Return;
}

constexpr bool IsExpired(const Hold& acHold, const double aNow) noexcept
{
    return aNow - acHold.Since >= kMaxHoldSeconds;
}

// What the server keeps of a hold it ended for running too long.
struct Rest
{
    uint32_t Holder{};
    double Until{};
};

constexpr Rest RestAfterExpiry(const Hold& acHold, const double aNow) noexcept
{
    return Rest{acHold.Holder, aNow + kRestSeconds};
}

constexpr bool IsResting(const Rest& acRest, const uint32_t aClaimant, const double aNow) noexcept
{
    return acRest.Holder != 0 && acRest.Holder == aClaimant && aNow < acRest.Until;
}

// One claim to hold, in the grabber's game.
struct Claim
{
    double AskedAt{};
    bool Granted{};
    double UpSince{-1.0};
};

enum class Step : uint8_t
{
    Wait,
    // Tell the server the hold ended.
    End,
    // Forget the claim: it was never granted, or the NPC went elsewhere.
    Forget,
};

// Once a frame for each claim: aExists, the NPC is still there; aOwned, this game runs it now.
constexpr Step StepClaim(Claim& aClaim, const double aNow, const bool aExists, const bool aOwned, const bool aDead, const bool aKnockRagdoll) noexcept
{
    if (!aExists)
        return Step::Forget;
    if (!aClaim.Granted)
    {
        if (!aOwned)
            return aNow - aClaim.AskedAt >= kGrantWithinSeconds ? Step::Forget : Step::Wait;
        aClaim.Granted = true;
    }
    if (!aOwned)
        return Step::Forget;
    if (aDead)
        return Step::End;
    if (aKnockRagdoll)
    {
        aClaim.UpSince = -1.0;
        return Step::Wait;
    }
    if (aClaim.UpSince < 0.0)
    {
        aClaim.UpSince = aNow;
        return Step::Wait;
    }
    return aNow - aClaim.UpSince >= kUpForSeconds ? Step::End : Step::Wait;
}
} // namespace GrabHold
