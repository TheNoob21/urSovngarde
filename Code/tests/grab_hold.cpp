#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <catch2/catch.hpp>

#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/RequestOwnershipClaim.h>
#include <Messages/RequestOwnershipTransfer.h>

#include <GrabHold.h>

#include <cmath>
#include <limits>

using namespace TiltedPhoques;

namespace
{
template <class T> T ClientRoundTrip(const T& acMessage)
{
    Buffer buffer(256);
    Buffer::Writer writer(&buffer);
    acMessage.Serialize(writer);
    Buffer::Reader reader(&buffer);
    auto pMessage = ClientMessageFactory{}.Extract(reader);
    REQUIRE(pMessage);
    REQUIRE(pMessage->GetOpcode() == T::Opcode);
    return *CastUnique<T>(std::move(pMessage));
}

GrabHold::ClaimToHold GoodClaim()
{
    GrabHold::ClaimToHold claim{};
    claim.SwitchOn = true;
    claim.Alive = true;
    claim.HasOwner = true;
    claim.OwnersInSameParty = true;
    claim.InRange = true;
    claim.EpochMatches = true;
    return claim;
}
} // namespace

TEST_CASE("claims to hold go over the wire unchanged", "[grab_hold][encoding]")
{
    SECTION("A claim says whether it is to hold a living NPC, after the flags before it")
    {
        for (const int cKind : {0, 1, 2, 3, 4})
        {
            RequestOwnershipClaim claim{};
            claim.ServerId = 0x1A;
            claim.ExpectedOwnershipEpoch = 0xFFFFFFFF;
            claim.Follower = cKind == 1;
            claim.KillingBlow = cKind == 2;
            claim.ForRiding = cKind == 3;
            claim.ForHolding = cKind == 4 || cKind == 0;
            const RequestOwnershipClaim cBack = ClientRoundTrip(claim);
            CHECK(cBack == claim);
            CHECK(cBack.ForHolding == claim.ForHolding);
            CHECK(cBack.ForRiding == claim.ForRiding);
        }
        RequestOwnershipClaim claim{};
        claim.ForHolding = true;
        auto ordinary = claim;
        ordinary.ForHolding = false;
        CHECK_FALSE(ordinary == claim);
    }

    SECTION("A release says the hold ended")
    {
        RequestOwnershipTransfer request{};
        request.ServerId = 0x99;
        request.OwnershipEpoch = 4;
        request.Reason = OwnershipReleaseReason::HoldEnded;
        const RequestOwnershipTransfer cBack = ClientRoundTrip(request);
        CHECK(cBack == request);
        CHECK(cBack.Reason == OwnershipReleaseReason::HoldEnded);
    }
}

TEST_CASE("a party member may hold a living NPC another party member's game runs", "[grab_hold]")
{
    CHECK(GrabHold::MayClaimToHold(GoodClaim()));

    // Each condition alone refuses it.
    const auto refused = [](auto aChange)
    {
        GrabHold::ClaimToHold claim = GoodClaim();
        aChange(claim);
        return !GrabHold::MayClaimToHold(claim);
    };
    CHECK(refused([](auto& c) { c.SwitchOn = false; }));
    CHECK(refused([](auto& c) { c.Alive = false; }));
    CHECK(refused([](auto& c) { c.IsPlayer = true; }));
    CHECK(refused([](auto& c) { c.IsMount = true; }));
    CHECK(refused([](auto& c) { c.Temporary = true; }));
    CHECK(refused([](auto& c) { c.HasOwner = false; }));
    CHECK(refused([](auto& c) { c.OwnedBySender = true; }));
    CHECK(refused([](auto& c) { c.OwnersInSameParty = false; }));
    CHECK(refused([](auto& c) { c.InRange = false; }));
    CHECK(refused([](auto& c) { c.EpochMatches = false; }));
    CHECK(refused([](auto& c) { c.OthersFollower = true; }));
    CHECK(refused([](auto& c) { c.FlyingDragon = true; }));
    CHECK(refused([](auto& c) { c.HeldByOther = true; }));
    CHECK(refused([](auto& c) { c.Resting = true; }));
}

TEST_CASE("a hold keeps the NPC from other claims, and gives it back or keeps it when it ends", "[grab_hold]")
{
    constexpr uint32_t cLeader = 1;
    constexpr uint32_t cMember = 2;
    constexpr uint32_t cThird = 3;

    // The leader's game ran it; the member grabbed it, and the hand-over gave epoch 5.
    GrabHold::Hold hold{cMember, cLeader, 5, false, 100.0};

    SECTION("While live, nobody else claims it, the leader's game included; a killing blow still may")
    {
        const bool cLive = GrabHold::IsLive(hold, cMember, 5);
        REQUIRE(cLive);
        CHECK(GrabHold::BlocksClaim(cLive, false, false));
        CHECK_FALSE(GrabHold::BlocksClaim(cLive, true, false));
        CHECK_FALSE(GrabHold::BlocksClaim(cLive, false, true));
        CHECK_FALSE(GrabHold::BlocksClaim(false, false, false));
    }

    SECTION("A second grabber's claim is refused while the first holds it")
    {
        GrabHold::ClaimToHold second = GoodClaim();
        second.HeldByOther = GrabHold::IsLive(hold, cMember, 5);
        CHECK_FALSE(GrabHold::MayClaimToHold(second));
        // The first grabber's claim with the epoch before the hand-over is stale too.
        GrabHold::ClaimToHold late = GoodClaim();
        late.EpochMatches = false;
        CHECK_FALSE(GrabHold::MayClaimToHold(late));
    }

    SECTION("A hand-over since ends the hold without a word")
    {
        CHECK_FALSE(GrabHold::IsLive(hold, cThird, 6));
        CHECK_FALSE(GrabHold::IsLive(hold, cMember, 6));
        CHECK_FALSE(GrabHold::IsLive(GrabHold::Hold{}, 0, 0));
        CHECK(GrabHold::OnEnd(hold, false, true, false, true) == GrabHold::End::Ignore);
    }

    SECTION("The holder's end gives it back to the game that ran it")
    {
        CHECK(GrabHold::OnEnd(hold, true, true, false, true) == GrabHold::End::Return);
        // Not from the holder: nothing.
        CHECK(GrabHold::OnEnd(hold, true, false, false, true) == GrabHold::End::Ignore);
    }

    SECTION("It stays by the usual rules: a body, a leader's hold, or a former owner who cannot take it back")
    {
        CHECK(GrabHold::OnEnd(hold, true, true, true, true) == GrabHold::End::Keep);
        CHECK(GrabHold::OnEnd(hold, true, true, false, false) == GrabHold::End::Keep);
        GrabHold::Hold leaders = hold;
        leaders.Keep = true;
        CHECK(GrabHold::OnEnd(leaders, true, true, false, true) == GrabHold::End::Keep);
        GrabHold::Hold noFormer = hold;
        noFormer.FormerOwner = 0;
        CHECK(GrabHold::OnEnd(noFormer, true, true, false, true) == GrabHold::End::Keep);
    }

    SECTION("A hold that runs too long is ended by the server, which counts as the holder's end")
    {
        CHECK_FALSE(GrabHold::IsExpired(hold, 100.0 + GrabHold::kMaxHoldSeconds - 0.5));
        CHECK(GrabHold::IsExpired(hold, 100.0 + GrabHold::kMaxHoldSeconds));
        CHECK(GrabHold::OnEnd(hold, true, true, false, true) == GrabHold::End::Return);

        // Its holder, still holding it, may not take it straight back; anybody else may, and so may it a little later.
        const double cEnded = 100.0 + GrabHold::kMaxHoldSeconds;
        const GrabHold::Rest cRest = GrabHold::RestAfterExpiry(hold, cEnded);
        CHECK(GrabHold::IsResting(cRest, cMember, cEnded + 1.0));
        CHECK_FALSE(GrabHold::IsResting(cRest, cThird, cEnded + 1.0));
        CHECK_FALSE(GrabHold::IsResting(cRest, cMember, cEnded + GrabHold::kRestSeconds));
        CHECK_FALSE(GrabHold::IsResting(GrabHold::Rest{}, 0, cEnded));
    }
}

TEST_CASE("the grabber's game ends its hold once the NPC is up again, or dead", "[grab_hold]")
{
    SECTION("Granted, held in its ragdoll, then up for a second")
    {
        GrabHold::Claim claim{0.0};
        CHECK(GrabHold::StepClaim(claim, 0.1, true, false, false, true) == GrabHold::Step::Wait);
        CHECK(GrabHold::StepClaim(claim, 0.3, true, true, false, true) == GrabHold::Step::Wait);
        CHECK(claim.Granted);
        CHECK(GrabHold::StepClaim(claim, 30.0, true, true, false, true) == GrabHold::Step::Wait);
        // Up, then back in its ragdoll before the second: the count starts again.
        CHECK(GrabHold::StepClaim(claim, 31.0, true, true, false, false) == GrabHold::Step::Wait);
        CHECK(GrabHold::StepClaim(claim, 31.5, true, true, false, true) == GrabHold::Step::Wait);
        CHECK(GrabHold::StepClaim(claim, 32.0, true, true, false, false) == GrabHold::Step::Wait);
        CHECK(GrabHold::StepClaim(claim, 32.9, true, true, false, false) == GrabHold::Step::Wait);
        CHECK(GrabHold::StepClaim(claim, 33.0, true, true, false, false) == GrabHold::Step::End);
    }

    SECTION("Dead while held: the hold ends, and the body stays with its grabber")
    {
        GrabHold::Claim claim{0.0};
        CHECK(GrabHold::StepClaim(claim, 0.2, true, true, false, true) == GrabHold::Step::Wait);
        CHECK(GrabHold::StepClaim(claim, 0.4, true, true, true, false) == GrabHold::Step::End);
    }

    SECTION("Never granted, taken away, or gone: forgotten, with nothing sent")
    {
        GrabHold::Claim refused{0.0};
        CHECK(GrabHold::StepClaim(refused, 4.9, true, false, false, true) == GrabHold::Step::Wait);
        CHECK(GrabHold::StepClaim(refused, GrabHold::kGrantWithinSeconds, true, false, false, true) == GrabHold::Step::Forget);

        GrabHold::Claim taken{0.0};
        CHECK(GrabHold::StepClaim(taken, 0.2, true, true, false, true) == GrabHold::Step::Wait);
        CHECK(GrabHold::StepClaim(taken, 0.4, true, false, false, true) == GrabHold::Step::Forget);

        GrabHold::Claim gone{0.0};
        CHECK(GrabHold::StepClaim(gone, 0.2, false, false, false, false) == GrabHold::Step::Forget);
    }
}
