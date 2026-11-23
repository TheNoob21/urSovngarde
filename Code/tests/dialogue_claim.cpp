#include <catch2/catch.hpp>

#include <DialogueClaim.h>

using namespace DialogueClaim;

namespace
{
constexpr double cAsked = 100.0;
} // namespace

TEST_CASE("talking to an npc another game runs waits for it, then opens the conversation here", "[dialogue_claim]")
{
    Claim claim{cAsked};

    REQUIRE(StepClaim(claim, cAsked + 0.2, true, false, false, false) == Step::Wait);
    // Handed over, but its AI is not ready yet.
    REQUIRE(StepClaim(claim, cAsked + 0.3, true, true, false, false) == Step::Wait);
    REQUIRE(StepClaim(claim, cAsked + 0.4, true, true, true, false) == Step::Activate);
    // Activated once only.
    REQUIRE(StepClaim(claim, cAsked + 0.5, true, true, true, false) == Step::Wait);
    REQUIRE(StepClaim(claim, cAsked + 0.6, true, true, true, true) == Step::Wait);
    REQUIRE(StepClaim(claim, cAsked + 30.0, true, true, true, true) == Step::Wait);
    // The conversation closed: the npc goes back.
    REQUIRE(StepClaim(claim, cAsked + 31.0, true, true, true, false) == Step::End);
}

TEST_CASE("a claim not granted in time talks to the copy as before, and a late grant goes back", "[dialogue_claim]")
{
    Claim claim{cAsked};

    REQUIRE(StepClaim(claim, cAsked + kGrantWaitSeconds - 0.01, true, false, false, false) == Step::Wait);
    REQUIRE(StepClaim(claim, cAsked + kGrantWaitSeconds, true, false, false, false) == Step::ActivateCopy);
    REQUIRE(StepClaim(claim, cAsked + 2.0, true, false, false, true) == Step::Wait);
    REQUIRE(StepClaim(claim, cAsked + 3.0, true, true, true, true) == Step::End);

    Claim never{cAsked};
    StepClaim(never, cAsked + kGrantWaitSeconds, true, false, false, false);
    REQUIRE(StepClaim(never, cAsked + kLateGrantSeconds, true, false, false, false) == Step::Forget);
}

TEST_CASE("a conversation that never opens hands the npc back", "[dialogue_claim]")
{
    Claim claim{cAsked};
    REQUIRE(StepClaim(claim, cAsked + 0.5, true, true, true, false) == Step::Activate);
    REQUIRE(StepClaim(claim, cAsked + 0.5 + kOpenWaitSeconds - 0.01, true, true, true, false) == Step::Wait);
    REQUIRE(StepClaim(claim, cAsked + 0.5 + kOpenWaitSeconds, true, true, true, false) == Step::End);
}

TEST_CASE("an npc handed over but never ready goes back unused", "[dialogue_claim]")
{
    Claim claim{cAsked};
    REQUIRE(StepClaim(claim, cAsked + 1.0, true, true, false, false) == Step::Wait);
    REQUIRE(StepClaim(claim, cAsked + kGrantWaitSeconds + kReadyWaitSeconds, true, true, false, false) == Step::End);
}

TEST_CASE("an npc gone or handed elsewhere mid-conversation is forgotten", "[dialogue_claim]")
{
    Claim gone{cAsked};
    REQUIRE(StepClaim(gone, cAsked + 0.1, false, false, false, false) == Step::Forget);

    Claim taken{cAsked};
    StepClaim(taken, cAsked + 0.2, true, true, true, false);
    StepClaim(taken, cAsked + 0.3, true, true, true, true);
    REQUIRE(StepClaim(taken, cAsked + 120.0, true, false, false, true) == Step::Forget);
}

TEST_CASE("only a living idle npc another game runs is claimed to talk", "[dialogue_claim]")
{
    Talk talk{};
    talk.Connected = true;
    talk.InParty = true;
    talk.Remote = true;
    talk.HasEpoch = true;
    REQUIRE(HoldsBackActivation(talk));

    auto with = [talk](auto aChange)
    {
        Talk changed = talk;
        aChange(changed);
        return HoldsBackActivation(changed);
    };
    REQUIRE_FALSE(with([](Talk& t) { t.Connected = false; }));
    REQUIRE_FALSE(with([](Talk& t) { t.InParty = false; }));
    REQUIRE_FALSE(with([](Talk& t) { t.Remote = false; }));
    REQUIRE_FALSE(with([](Talk& t) { t.RemotePlayer = true; }));
    REQUIRE_FALSE(with([](Talk& t) { t.Dead = true; }));
    REQUIRE_FALSE(with([](Talk& t) { t.Mount = true; }));
    REQUIRE_FALSE(with([](Talk& t) { t.InCombat = true; }));
    REQUIRE_FALSE(with([](Talk& t) { t.PlayerSneaking = true; }));
    REQUIRE_FALSE(with([](Talk& t) { t.HasEpoch = false; }));
    REQUIRE_FALSE(with([](Talk& t) { t.OthersFollower = true; }));
    REQUIRE_FALSE(with([](Talk& t) { t.InScene = true; }));
    REQUIRE_FALSE(with([](Talk& t) { t.Temporary = true; }));
    REQUIRE_FALSE(with([](Talk& t) { t.FlyingDragon = true; }));
    REQUIRE_FALSE(with([](Talk& t) { t.Resting = true; }));
}

TEST_CASE("the server taking a talk's npc back makes this player rest before claiming it again", "[dialogue_claim]")
{
    REQUIRE(kRestSeconds == GrabHold::kRestSeconds);

    Claim talked{cAsked};
    StepClaim(talked, cAsked + 0.2, true, true, true, false);
    StepClaim(talked, cAsked + 0.3, true, true, true, true);
    REQUIRE(StepClaim(talked, cAsked + 120.0, true, false, false, true) == Step::Forget);
    REQUIRE(StartsRest(talked, true, false));

    // Gone, never activated, or talked past: nothing to rest from.
    REQUIRE_FALSE(StartsRest(talked, false, false));
    REQUIRE_FALSE(StartsRest(Claim{cAsked}, true, false));
    Claim past{cAsked};
    StepClaim(past, cAsked + kGrantWaitSeconds, true, false, false, false);
    REQUIRE_FALSE(StartsRest(past, true, false));
    // Handed back by this game itself: it still runs it at that moment.
    REQUIRE_FALSE(StartsRest(talked, true, true));
}
