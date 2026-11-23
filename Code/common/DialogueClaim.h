#pragma once

// Talking to an NPC another game runs: this game asks the server for it first, so the conversation runs where the NPC's AI
// does and the other players see the NPC talk through its copy. Plain values, so the rules are unit-tested.
//
// The player's activation of the copy is held back and a claim goes out (the server's hold claim: a party member may run
// another party member's living NPC for a while, and hands it back when done, GrabHold). Once this game runs the NPC, it is
// activated again and the game opens the conversation. When the conversation ends the NPC goes back. A claim not granted in
// time (not in a party, holds switched off, someone else holds it) talks to the copy as before.

#include <GrabHold.h>

#include <cstdint>

namespace DialogueClaim
{
// How long the activation waits for the server to hand the NPC over, in seconds; then the copy is talked to as before.
inline constexpr double kGrantWaitSeconds = 1.5;
// How long, once this game runs the NPC, its AI may take to be ready to talk, in seconds; then the NPC goes back unused.
inline constexpr double kReadyWaitSeconds = 1.5;
// How long the conversation may take to open after the activation, in seconds; then the NPC goes back.
inline constexpr double kOpenWaitSeconds = 2.0;
// How long after the server took a talk's NPC back (its hold ran too long) this player waits to claim it again, in seconds:
// the server's own rest, so the claim is not sent only to be refused (GrabHold::IsResting).
inline constexpr double kRestSeconds = GrabHold::kRestSeconds;
// How long a claim that was talked past is kept, in seconds, so a grant that comes late is handed back at once.
inline constexpr double kLateGrantSeconds = 5.0;

struct Claim
{
    double AskedAt{};
    double ActivatedAt{-1.0}; // when the conversation was asked for here; below 0 while still waiting
    bool Talked{};            // the conversation opened with this NPC
    bool TalkedPast{};        // not granted in time: the copy was talked to as before
};

enum class Step : uint8_t
{
    Wait,
    // This game runs the NPC now: activate it again, so the game opens the conversation.
    Activate,
    // Not granted in time: activate the copy again, as without a claim.
    ActivateCopy,
    // Hand the NPC back to the server: the conversation ended, never opened, or a grant came too late.
    End,
    // Forget the claim: the NPC is gone, went elsewhere, or no grant came.
    Forget,
};

// Once a frame for each claim. aExists: the NPC is still there. aOwned: this game runs it now. aReady: its AI is loaded and
// can talk. aTalking: the conversation is open with it.
constexpr Step StepClaim(Claim& aClaim, const double aNow, const bool aExists, const bool aOwned, const bool aReady, const bool aTalking) noexcept
{
    if (!aExists)
        return Step::Forget;

    const double cSinceAsked = aNow - aClaim.AskedAt;
    if (aClaim.TalkedPast)
    {
        if (aOwned)
            return Step::End;
        return cSinceAsked >= kLateGrantSeconds ? Step::Forget : Step::Wait;
    }

    if (aClaim.ActivatedAt < 0.0)
    {
        if (aOwned && aReady)
        {
            aClaim.ActivatedAt = aNow;
            return Step::Activate;
        }
        if (aOwned)
            return cSinceAsked >= kGrantWaitSeconds + kReadyWaitSeconds ? Step::End : Step::Wait;
        if (cSinceAsked >= kGrantWaitSeconds)
        {
            aClaim.TalkedPast = true;
            return Step::ActivateCopy;
        }
        return Step::Wait;
    }

    // Handed elsewhere meanwhile (the hold ran too long, this player left the party): nothing to give back.
    if (!aOwned)
        return Step::Forget;
    if (aTalking)
    {
        aClaim.Talked = true;
        return Step::Wait;
    }
    if (aClaim.Talked || aNow - aClaim.ActivatedAt >= kOpenWaitSeconds)
        return Step::End;
    return Step::Wait;
}

// Set while this game activates the NPC again for the player, so the Activate hook does not hold that one back too.
inline thread_local bool t_activatingToTalk = false;

class ScopedActivatingToTalk
{
public:
    ScopedActivatingToTalk() noexcept
        : m_was(t_activatingToTalk)
    {
        t_activatingToTalk = true;
    }
    ~ScopedActivatingToTalk() noexcept { t_activatingToTalk = m_was; }
    ScopedActivatingToTalk(const ScopedActivatingToTalk&) = delete;
    ScopedActivatingToTalk& operator=(const ScopedActivatingToTalk&) = delete;

private:
    bool m_was;
};

// Whether the server took the NPC back while this game was talking to it, so this player rests before claiming it again.
constexpr bool StartsRest(const Claim& acClaim, const bool aExists, const bool aOwned) noexcept
{
    return acClaim.ActivatedAt >= 0.0 && !acClaim.TalkedPast && aExists && !aOwned;
}

// Whether the player's activation of a copy is held back for a claim. Only a living, idle NPC another game runs is talked to:
// sneaking is a pickpocket, a body is looted, a horse is ridden, a fight is no conversation, a player copy is nobody's to claim,
// another player's follower is her player's, and an NPC in a scene may be playing a quest's lines in its own game: the copy is
// talked to then, as before. Nor is an NPC the server would refuse anyway (a temporary one, a dragon in flight, one this player's
// last talk ran too long with a moment ago): the copy is talked to at once instead of after the wait.
struct Talk
{
    bool Connected{};
    bool InParty{};
    bool Remote{};       // another game runs it
    bool RemotePlayer{}; // a copy of another player
    bool Dead{};
    bool Mount{};
    bool InCombat{};
    bool PlayerSneaking{};
    bool OthersFollower{}; // another player's follower
    bool InScene{};        // a scene is playing with it here
    bool Temporary{};      // placed at run time: not the same reference in another game
    bool FlyingDragon{};   // a dragon in the air, which the server never hands over
    bool Resting{};        // this player's last talk with it ran too long a moment ago
    bool HasEpoch{};       // its copy knows the ownership epoch the claim needs
};

constexpr bool HoldsBackActivation(const Talk& acTalk) noexcept
{
    return acTalk.Connected && acTalk.InParty && acTalk.Remote && !acTalk.RemotePlayer && !acTalk.Dead && !acTalk.Mount && !acTalk.InCombat && !acTalk.PlayerSneaking &&
           !acTalk.OthersFollower && !acTalk.InScene && !acTalk.Temporary && !acTalk.FlyingDragon && !acTalk.Resting && acTalk.HasEpoch;
}
} // namespace DialogueClaim
