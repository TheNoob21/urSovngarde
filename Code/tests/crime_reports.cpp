#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <catch2/catch.hpp>

#include <Messages/ClientMessageFactory.h>
#include <Messages/RequestOwnershipClaim.h>

#include <CrimeReports.h>
#include <VRAddressOverrides.h>

using namespace CrimeReports;

TEST_CASE("A crime witness may fight a copy only if it is not a guard", "[crime_reports]")
{
    CHECK(WitnessMayFight(false));
    CHECK_FALSE(WitnessMayFight(true));
}

TEST_CASE("A witness calls out a copy's theft only if it is not a guard", "[crime_reports]")
{
    CHECK(WitnessMayCallOut(false));
    CHECK_FALSE(WitnessMayCallOut(true));
}

TEST_CASE("Only a real assault or murder of a guard keeps the guard combat response", "[crime_reports]")
{
    CHECK(GuardWasAttacked(Kind::kAssault, true));
    CHECK(GuardWasAttacked(Kind::kMurder, true));
    CHECK_FALSE(GuardWasAttacked(Kind::kTheft, true));
    CHECK_FALSE(GuardWasAttacked(Kind::kPickpocket, true));
    CHECK_FALSE(GuardWasAttacked(Kind::kTrespass, true));
    CHECK_FALSE(GuardWasAttacked(Kind::kAssault, false));
    CHECK_FALSE(GuardWasAttacked(Kind::kMurder, false));
}

TEST_CASE("A guard's existing combat or target is preserved through a crime replay", "[crime_reports]")
{
    CHECK(GuardCombatAfterCrime(false, false, true) == GuardCombat::kStop);
    CHECK(GuardCombatAfterCrime(true, false, true) == GuardCombat::kRestoreTarget);
    CHECK(GuardCombatAfterCrime(false, true, true) == GuardCombat::kRestoreTarget);
    CHECK(GuardCombatAfterCrime(true, true, true) == GuardCombat::kRestoreTarget);
    CHECK(GuardCombatAfterCrime(false, false, false) == GuardCombat::kKeep);
    CHECK(GuardCombatAfterCrime(true, false, false) == GuardCombat::kKeep);
    CHECK(GuardCombatAfterCrime(false, true, false) == GuardCombat::kKeep);
    CHECK(GuardCombatAfterCrime(true, true, false) == GuardCombat::kKeep);
}

TEST_CASE("Only this game's player's own crime is reported, never one replayed for another player", "[crime_reports]")
{
    CHECK(ShouldReport(true, false));
    CHECK_FALSE(ShouldReport(false, false)); // an NPC's or another player's copy's crime
    CHECK_FALSE(ShouldReport(true, true));   // cannot happen, but a replay is never sent back
    CHECK_FALSE(ShouldReport(false, true));  // the replay of another player's theft here
}

TEST_CASE("The witness collector keeps each copy once, sixteen at most, and only while on", "[crime_reports]")
{
    WitnessCollector witnesses;
    witnesses.Add(7);
    CHECK(witnesses.Count() == 0); // off

    witnesses.Start();
    witnesses.Add(7);
    witnesses.Add(7);
    witnesses.Add(0);
    witnesses.Add(8);
    REQUIRE(witnesses.Count() == 2);
    CHECK(witnesses.Ids()[0] == 7);
    CHECK(witnesses.Ids()[1] == 8);

    for (uint32_t id = 100; id < 140; ++id)
        witnesses.Add(id);
    CHECK(witnesses.Count() == kMaxWitnesses);

    witnesses.Stop();
    witnesses.Add(999);
    CHECK(witnesses.Count() == kMaxWitnesses);

    // A new alarm starts empty.
    witnesses.Start();
    CHECK(witnesses.Count() == 0);
}

TEST_CASE("The server passes a report only to the owner of each witness, in the offender's party and near", "[crime_reports]")
{
    CHECK(ServerTakesWitness(true, false, true, false, true, true));
    CHECK_FALSE(ServerTakesWitness(false, false, true, false, true, true)); // not a character
    CHECK_FALSE(ServerTakesWitness(true, true, true, false, true, true));   // a player's character
    CHECK_FALSE(ServerTakesWitness(true, false, false, false, true, true)); // no one runs it
    CHECK_FALSE(ServerTakesWitness(true, false, true, true, true, true));   // the offender's own game runs it
    CHECK_FALSE(ServerTakesWitness(true, false, true, false, false, true)); // another party
    CHECK_FALSE(ServerTakesWitness(true, false, true, false, true, false)); // too far
}

TEST_CASE("The witness's game runs the alarm only for another player's theft that its own NPC still sees", "[crime_reports]")
{
    CHECK(ReceiverRuns(true, true, false, false, true, true, true));
    CHECK_FALSE(ReceiverRuns(false, true, false, false, true, true, true)); // a kind not taken yet
    CHECK_FALSE(ReceiverRuns(true, false, false, false, true, true, true)); // no copy of the offender here
    CHECK_FALSE(ReceiverRuns(true, true, true, false, true, true, true));   // this game's own player
    CHECK_FALSE(ReceiverRuns(true, true, false, true, true, true, true));   // a teammate here
    CHECK_FALSE(ReceiverRuns(true, true, false, false, false, true, true)); // its NPC no longer sees the copy
    CHECK_FALSE(ReceiverRuns(true, true, false, false, true, false, true)); // the item is unknown here
    CHECK_FALSE(ReceiverRuns(true, true, false, false, true, true, false)); // the owner is unknown here
}

TEST_CASE("A player sends at most kPerSecond reports a second", "[crime_reports]")
{
    RateCount rate;
    const auto cStart = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kPerSecond; ++i)
        CHECK(rate.Take(cStart));
    CHECK_FALSE(rate.Take(cStart + std::chrono::milliseconds(500)));
    CHECK(rate.Take(cStart + std::chrono::seconds(1)));
}

TEST_CASE("The kinds are the game's own crime numbers", "[crime_reports]")
{
    CHECK(static_cast<uint8_t>(Kind::kTheft) == 0);
    CHECK(static_cast<uint8_t>(Kind::kPickpocket) == 1);
    CHECK(static_cast<uint8_t>(Kind::kTrespass) == 2);
    CHECK(static_cast<uint8_t>(Kind::kAssault) == 3);
    CHECK(static_cast<uint8_t>(Kind::kMurder) == 4);
}

TEST_CASE("Every kind but trespass is taken, each with its own reaction", "[crime_reports]")
{
    CHECK(Taken(Kind::kTheft));
    CHECK(Taken(Kind::kPickpocket));
    CHECK(Taken(Kind::kAssault));
    CHECK(Taken(Kind::kMurder));
    CHECK_FALSE(Taken(Kind::kTrespass));
    CHECK_FALSE(Taken(uint8_t(5)));
    CHECK_FALSE(Taken(uint8_t(7)));

    // Native StealAlarm rejects an ordinary non-player before querying witnesses, so a theft is called out (no fight), as in the base game.
    CHECK(ReactionOf(Kind::kTheft) == Reaction::kCallOut);
    CHECK(ReactionOf(Kind::kTheft) != Reaction::kStartCombat);
    CHECK(ReactionOf(Kind::kAssault) == Reaction::kReplayAssault);
    // The murder alarm does nothing for another player: the assault alarm on the dead victim stands in for it.
    CHECK(ReactionOf(Kind::kMurder) == Reaction::kReplayAssault);
    // The pickpocket alarm asks about the game's own player only: the witnesses fight or flee instead.
    CHECK(ReactionOf(Kind::kPickpocket) == Reaction::kStartCombat);
    CHECK(ReactionOf(Kind::kTrespass) == Reaction::kNone);
}

TEST_CASE("VR finds the assault, murder and pickpocket alarms where the VR game has them", "[crime_reports]")
{
    // SE ids, as VR looks them up: the functions after StealAlarm, matched against flat AE 37423-37426 by their exception-table
    // entries (the same prologues, in the same order) and Ghidra on flat.
    const auto cOffsetOf = [](const uint32_t aId) -> uint32_t
    {
        for (const auto& cEntry : kVRAddressOverrides)
            if (cEntry.id == aId)
                return cEntry.offset;
        return 0;
    };
    CHECK(cOffsetOf(36427) == 0x5e5dc0); // Actor::StealAlarm
    CHECK(cOffsetOf(36428) == 0x5e67b0); // the pickpocket alarm
    CHECK(cOffsetOf(36429) == 0x5e6e60); // the assault alarm's entry
    CHECK(cOffsetOf(36431) == 0x5e7570); // the murder alarm
}

TEST_CASE("the offender's game asks for the living guards that saw its player's crime", "[crime_reports]")
{
    CHECK(CrimeReports::OffenderAsksForWitness(true, true));
    // A civilian (who reacts in its own game), a dead guard.
    CHECK_FALSE(CrimeReports::OffenderAsksForWitness(false, true));
    CHECK_FALSE(CrimeReports::OffenderAsksForWitness(true, false));
}

TEST_CASE("the server gives a witness only to the player whose report named it, for a while", "[crime_reports]")
{
    using namespace std::chrono_literals;
    WitnessClaim claim{};
    claim.On = claim.ForCrime = claim.NamedBySender = claim.EpochAsNamed = claim.Alive = true;
    CHECK(ServerGivesWitness(claim));
    claim.SinceNamed = kWitnessClaimFor - 1ms;
    CHECK(ServerGivesWitness(claim));
    claim.SinceNamed = kWitnessClaimFor;
    CHECK_FALSE(ServerGivesWitness(claim));
    claim.SinceNamed = -1s; // a time from before the report
    CHECK_FALSE(ServerGivesWitness(claim));
    claim.SinceNamed = 1s;
    // Off on the server, an ordinary claim, never named by the claimant's report, an epoch since changed, a dead witness.
    for (bool WitnessClaim::* pGate : {&WitnessClaim::On, &WitnessClaim::ForCrime, &WitnessClaim::NamedBySender, &WitnessClaim::EpochAsNamed, &WitnessClaim::Alive})
    {
        WitnessClaim refused = claim;
        refused.*pGate = false;
        CHECK_FALSE(ServerGivesWitness(refused));
    }
}

namespace
{
// The server's answer to a claim on aWitness from aClaimant, as CharacterService::OnOwnershipClaimRequest gives it, with the guard alive.
bool GivenFor(WitnessBook& aBook, bool aOn, bool aForCrime, uint32_t aWitness, uint32_t aClaimant, uint32_t aEpoch, std::chrono::steady_clock::time_point aNow)
{
    if (!aForCrime || !ServerGivesWitness(aBook.ClaimOf(aOn, aForCrime, aWitness, aClaimant, aEpoch, true, aNow)))
        return false;
    aBook.Take(aWitness, aClaimant);
    return true;
}
} // namespace

TEST_CASE("an ordinary claim never spends a crime report, and two offenders keep their own reports", "[crime_reports]")
{
    using namespace std::chrono_literals;
    const std::chrono::steady_clock::time_point cT0{std::chrono::hours(1)};
    constexpr uint32_t cGuard = 0x40;
    constexpr uint32_t cOffender = 1;
    constexpr uint32_t cOther = 2;
    WitnessBook book;

    // The offender's report names the guard at epoch 5. The offender's game then claims it for something else (a leader's claim).
    book.Note(cGuard, cOffender, 5, cT0);
    CHECK_FALSE(GivenFor(book, true, false, cGuard, cOffender, 5, cT0 + 1s));
    REQUIRE(book.Find(cGuard, cOffender));

    // Another player's later report of the same guard does not replace the first.
    book.Note(cGuard, cOther, 5, cT0 + 2s);
    CHECK(book.Size() == 2);
    // A claim for the crime by someone whose report did not name it is refused.
    CHECK_FALSE(GivenFor(book, true, true, cGuard, 3, 5, cT0 + 2s));

    // The offender's claim for the crime takes it, and only its own report.
    CHECK(GivenFor(book, true, true, cGuard, cOffender, 5, cT0 + 3s));
    CHECK_FALSE(book.Find(cGuard, cOffender));
    REQUIRE(book.Find(cGuard, cOther));
    // The guard changed hands since the other report named it: that report no longer gives it.
    CHECK_FALSE(GivenFor(book, true, true, cGuard, cOther, 6, cT0 + 3s));

    // Off on the server, a claim for the crime gives nothing either.
    book.Note(0x41, cOffender, 2, cT0 + 4s);
    CHECK_FALSE(GivenFor(book, false, true, 0x41, cOffender, 2, cT0 + 4s));

    // Old reports go when new ones come in.
    book.Note(0x42, cOffender, 1, cT0 + 4s + kWitnessClaimFor);
    CHECK(book.Size() == 1);
}

namespace
{
enum class Answer
{
    Witness,
    Refused,
    OtherRule,
};

// The first step of CharacterService::OnOwnershipClaimRequest for a claim as the server reads it off the wire.
Answer Dispatch(WitnessBook& aBook, bool aOn, const RequestOwnershipClaim& acClaim, uint32_t aClaimant, std::chrono::steady_clock::time_point aNow)
{
    if (!ByWitnessRuleOnly(acClaim))
        return Answer::OtherRule;
    return GivenFor(aBook, aOn, true, acClaim.ServerId, aClaimant, acClaim.ExpectedOwnershipEpoch, aNow) ? Answer::Witness : Answer::Refused;
}

RequestOwnershipClaim OffTheWire(const RequestOwnershipClaim& acClaim)
{
    TiltedPhoques::Buffer buffer(64);
    TiltedPhoques::Buffer::Writer writer(&buffer);
    acClaim.Serialize(writer);
    TiltedPhoques::Buffer::Reader reader(&buffer);
    auto pMessage = ClientMessageFactory{}.Extract(reader);
    REQUIRE(pMessage);
    return *TiltedPhoques::CastUnique<RequestOwnershipClaim>(std::move(pMessage));
}
} // namespace

TEST_CASE("a claim for a crime's witness is answered by the witness rule alone, whatever else it says", "[crime_reports]")
{
    using namespace std::chrono_literals;
    const std::chrono::steady_clock::time_point cT0{std::chrono::hours(1)};
    constexpr uint32_t cGuard = 0x40;
    constexpr uint32_t cOffender = 1;

    // A guard the offender's report named, claimed for the crime with each other purpose a client could also set (a follower guard).
    for (bool RequestOwnershipClaim::* pAlso :
         {&RequestOwnershipClaim::Follower, &RequestOwnershipClaim::KillingBlow, &RequestOwnershipClaim::ForHolding, &RequestOwnershipClaim::ForRiding})
    {
        RequestOwnershipClaim claim{};
        claim.ServerId = cGuard;
        claim.ExpectedOwnershipEpoch = 5;
        claim.ForCrime = true;
        claim.*pAlso = true;
        const RequestOwnershipClaim cRead = OffTheWire(claim);

        WitnessBook book;
        book.Note(cGuard, cOffender, 5, cT0);
        // Gameplay:bCrimeGuardHandoff off: refused, and no other rule gets it.
        CHECK(Dispatch(book, false, cRead, cOffender, cT0 + 1s) == Answer::Refused);
        REQUIRE(book.Find(cGuard, cOffender));
        // On: given by the witness rule, once.
        CHECK(Dispatch(book, true, cRead, cOffender, cT0 + 1s) == Answer::Witness);
        CHECK(Dispatch(book, true, cRead, cOffender, cT0 + 1s) == Answer::Refused);

        // The same claim without the crime goes by the other rules, and leaves the report alone.
        claim.ForCrime = false;
        book.Note(cGuard, cOffender, 5, cT0);
        CHECK(Dispatch(book, true, OffTheWire(claim), cOffender, cT0 + 1s) == Answer::OtherRule);
        CHECK(book.Find(cGuard, cOffender));
    }
}

TEST_CASE("a claim says whether it is for a crime's witness, after the flags before it", "[crime_reports][encoding.packets]")
{
    for (const bool cForCrime : {false, true})
    {
        RequestOwnershipClaim claim{};
        claim.ServerId = 0x40;
        claim.ExpectedOwnershipEpoch = 5;
        claim.ForHolding = !cForCrime;
        claim.ForCrime = cForCrime;
        TiltedPhoques::Buffer buffer(64);
        TiltedPhoques::Buffer::Writer writer(&buffer);
        claim.Serialize(writer);
        TiltedPhoques::Buffer::Reader reader(&buffer);
        auto pMessage = ClientMessageFactory{}.Extract(reader);
        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == RequestOwnershipClaim::Opcode);
        const RequestOwnershipClaim cBack = *TiltedPhoques::CastUnique<RequestOwnershipClaim>(std::move(pMessage));
        CHECK(cBack == claim);
        CHECK(cBack.ForCrime == cForCrime);
        CHECK(cBack.ForHolding == !cForCrime);
    }
}
