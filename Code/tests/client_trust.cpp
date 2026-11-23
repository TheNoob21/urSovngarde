#include <catch2/catch.hpp>

#include <ClientTrust.h>
#include <KillCredit.h>

#include <limits>
#include <set>
#include <string>

using namespace ClientTrust;

namespace
{
HealthFacts Stranger()
{
    HealthFacts facts;
    facts.ActorKnown = true;
    facts.InRange = true;
    return facts;
}
} // namespace

TEST_CASE("a health change from a game that runs the actor is taken, near or far", "[client_trust]")
{
    HealthFacts facts = Stranger();
    facts.SenderOwnsTarget = true;
    facts.InRange = false;
    for (const bool player : {false, true})
    {
        facts.TargetIsPlayer = player;
        for (float delta : {-50.f, 12.5f, 0.f})
        {
            float taken = delta;
            REQUIRE(JudgeHealthChange(facts, taken) == Refusal::kNone);
            REQUIRE(taken == delta);
        }
    }
}

TEST_CASE("a health change of no number, or on no actor, is refused", "[client_trust]")
{
    HealthFacts facts = Stranger();
    float delta = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(JudgeHealthChange(facts, delta) == Refusal::kNotFinite);
    delta = std::numeric_limits<float>::infinity();
    REQUIRE(JudgeHealthChange(facts, delta) == Refusal::kNotFinite);
    delta = -std::numeric_limits<float>::infinity();
    REQUIRE(JudgeHealthChange(facts, delta) == Refusal::kNotFinite);

    facts.ActorKnown = false;
    delta = -10.f;
    REQUIRE(JudgeHealthChange(facts, delta) == Refusal::kUnknownActor);
}

TEST_CASE("another game's hit on an NPC is taken only from a game that has it in range", "[client_trust]")
{
    HealthFacts facts = Stranger();
    float delta = -35.f;
    REQUIRE(JudgeHealthChange(facts, delta) == Refusal::kNone);
    REQUIRE(delta == -35.f);

    facts.InRange = false;
    REQUIRE(JudgeHealthChange(facts, delta) == Refusal::kOutOfRange);
}

TEST_CASE("another game's hit on a player's character needs PvP on", "[client_trust]")
{
    HealthFacts facts = Stranger();
    facts.TargetIsPlayer = true;
    float delta = -20.f;
    REQUIRE(JudgeHealthChange(facts, delta) == Refusal::kPvpOff);

    facts.PvpOn = true;
    REQUIRE(JudgeHealthChange(facts, delta) == Refusal::kNone);
    REQUIRE(delta == -20.f);

    // Still only from a game that has the player in range.
    facts.InRange = false;
    REQUIRE(JudgeHealthChange(facts, delta) == Refusal::kOutOfRange);
}

TEST_CASE("no other game heals or sets a player's character", "[client_trust]")
{
    HealthFacts facts = Stranger();
    facts.TargetIsPlayer = true;
    facts.PvpOn = true;
    for (float delta : {40.f, 0.f})
    {
        float taken = delta;
        REQUIRE(JudgeHealthChange(facts, taken) == Refusal::kNotOwner);
    }

    // An NPC's report is not so: the game that runs it may send what another game did to it.
    facts.TargetIsPlayer = false;
    float heal = 40.f;
    REQUIRE(JudgeHealthChange(facts, heal) == Refusal::kNone);
}

TEST_CASE("a health change is cut to the largest, and a killing blow passes whole", "[client_trust]")
{
    HealthFacts facts = Stranger();
    float delta = -5.0e9f;
    REQUIRE(JudgeHealthChange(facts, delta) == Refusal::kNone);
    REQUIRE(delta == -kMaxHealthChange);

    delta = 3.0e7f;
    facts.SenderOwnsTarget = true;
    REQUIRE(JudgeHealthChange(facts, delta) == Refusal::kNone);
    REQUIRE(delta == kMaxHealthChange);

    facts.SenderOwnsTarget = false;
    delta = -KillCredit::kDeathWinsBlow;
    REQUIRE(JudgeHealthChange(facts, delta) == Refusal::kNone);
    REQUIRE(delta == -KillCredit::kDeathWinsBlow);
}

TEST_CASE("a cast, a shot or a spell taken off is the game's that runs the actor", "[client_trust]")
{
    REQUIRE(JudgeOwnedActor(true, true) == Refusal::kNone);
    REQUIRE(JudgeOwnedActor(true, false) == Refusal::kNotOwner);
    REQUIRE(JudgeOwnedActor(false, false) == Refusal::kUnknownActor);
    REQUIRE(JudgeOwnedActor(false, true) == Refusal::kUnknownActor);
}

TEST_CASE("an effect is reported by the game that runs its caster or its target", "[client_trust]")
{
    REQUIRE(JudgeEffect(true, false, true, 25.f) == Refusal::kNone);
    REQUIRE(JudgeEffect(true, true, false, 25.f) == Refusal::kNone);
    REQUIRE(JudgeEffect(true, true, true, 0.f) == Refusal::kNone);
    REQUIRE(JudgeEffect(true, false, false, 25.f) == Refusal::kNotOwner);
    REQUIRE(JudgeEffect(false, true, true, 25.f) == Refusal::kUnknownActor);
    REQUIRE(JudgeEffect(true, true, true, std::numeric_limits<float>::quiet_NaN()) == Refusal::kNotFinite);
    REQUIRE(JudgeEffect(true, true, true, std::numeric_limits<float>::infinity()) == Refusal::kNotFinite);
}

TEST_CASE("a shot is the game's that runs the shooter, from where the shooter is", "[client_trust]")
{
    REQUIRE(JudgeShot(true, true, 120.f, 1.f, 2.f, 3.f, 1.f, 1.f) == Refusal::kNone);
    // The server has no position for the shooter yet.
    REQUIRE(JudgeShot(true, true, -1.f, 1.f, 2.f, 3.f, 1.f, 1.f) == Refusal::kNone);
    REQUIRE(JudgeShot(true, true, kMaxShotOriginDistance, 1.f, 2.f, 3.f, 1.f, 1.f) == Refusal::kNone);
    REQUIRE(JudgeShot(true, true, kMaxShotOriginDistance + 1.f, 1.f, 2.f, 3.f, 1.f, 1.f) == Refusal::kOutOfRange);
    REQUIRE(JudgeShot(true, false, 120.f, 1.f, 2.f, 3.f, 1.f, 1.f) == Refusal::kNotOwner);
    REQUIRE(JudgeShot(false, false, 120.f, 1.f, 2.f, 3.f, 1.f, 1.f) == Refusal::kUnknownActor);
}

TEST_CASE("a shot with a number that is not finite is refused", "[client_trust]")
{
    const float cNaN = std::numeric_limits<float>::quiet_NaN();
    const float cInf = std::numeric_limits<float>::infinity();
    REQUIRE(JudgeShot(true, true, 0.f, cNaN, 2.f, 3.f, 1.f, 1.f) == Refusal::kNotFinite);
    REQUIRE(JudgeShot(true, true, 0.f, 1.f, cInf, 3.f, 1.f, 1.f) == Refusal::kNotFinite);
    REQUIRE(JudgeShot(true, true, 0.f, 1.f, 2.f, cNaN, 1.f, 1.f) == Refusal::kNotFinite);
    REQUIRE(JudgeShot(true, true, 0.f, 1.f, 2.f, 3.f, cInf, 1.f) == Refusal::kNotFinite);
    REQUIRE(JudgeShot(true, true, 0.f, 1.f, 2.f, 3.f, 1.f, cNaN) == Refusal::kNotFinite);
}

TEST_CASE("anyone near an NPC may start its words, in no more than the largest text", "[client_trust]")
{
    REQUIRE(JudgeSpeech(true, false, true, 40) == Refusal::kNone);
    REQUIRE(JudgeSpeech(true, true, false, 40) == Refusal::kNone);
    REQUIRE(JudgeSpeech(true, false, false, 40) == Refusal::kOutOfRange);
    REQUIRE(JudgeSpeech(false, true, true, 40) == Refusal::kUnknownActor);
    REQUIRE(JudgeSpeech(true, true, true, kMaxSpeechLength) == Refusal::kNone);
    REQUIRE(JudgeSpeech(true, true, true, kMaxSpeechLength + 1) == Refusal::kTooLong);
}

TEST_CASE("an actor value is taken only as a number", "[client_trust]")
{
    REQUIRE(IsSaneValue(0.f));
    REQUIRE(IsSaneValue(-300.f));
    REQUIRE_FALSE(IsSaneValue(std::numeric_limits<float>::quiet_NaN()));
    REQUIRE_FALSE(IsSaneValue(std::numeric_limits<float>::infinity()));
}

TEST_CASE("the first position of an actor, and any after a hand-over, is taken", "[client_trust]")
{
    HeldJump held;
    REQUIRE(JudgeStep({0.f, 0.f, 0.f}, 0, {100000.f, -90000.f, 50.f}, 5000, false, held) == Step::kTake);
    REQUIRE_FALSE(held.Held);
}

TEST_CASE("a position outside the world is refused", "[client_trust]")
{
    HeldJump held;
    const float cNaN = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(JudgeStep({}, 0, {cNaN, 0.f, 0.f}, 100, false, held) == Step::kRefuse);
    REQUIRE(JudgeStep({}, 100, {0.f, std::numeric_limits<float>::infinity(), 0.f}, 200, true, held) == Step::kRefuse);
    REQUIRE(JudgeStep({}, 100, {0.f, 0.f, kMaxCoordinate * 2.f}, 200, true, held) == Step::kRefuse);
    REQUIRE(JudgeStep({}, 100, {-kMaxCoordinate * 2.f, 0.f, 0.f}, 200, false, held) == Step::kRefuse);
    REQUIRE(JudgeStep({}, 100, {kMaxCoordinate, -kMaxCoordinate, 0.f}, 200, true, held) == Step::kTake);
}

TEST_CASE("walking, sprinting, riding and diving are taken at the owner's rate", "[client_trust]")
{
    HeldJump held;
    // A sprint is about 700 units a second and a dragon's dive 4000; reports come every 33 ms.
    REQUIRE(JudgeStep({0.f, 0.f, 0.f}, 1000, {24.f, 0.f, 0.f}, 1033, false, held) == Step::kTake);
    REQUIRE(JudgeStep({0.f, 0.f, 0.f}, 1000, {0.f, 0.f, -135.f}, 1033, false, held) == Step::kTake);
    // An NPC at ten a second.
    REQUIRE(JudgeStep({0.f, 0.f, 0.f}, 1000, {0.f, 400.f, 0.f}, 1100, false, held) == Step::kTake);
    // A long gap lets an actor have gone as far as its speed takes it.
    REQUIRE(JudgeStep({0.f, 0.f, 0.f}, 1000, {20000.f, 0.f, 0.f}, 4000, false, held) == Step::kTake);
    REQUIRE_FALSE(held.Held);
}

TEST_CASE("a jump beyond the speeds is held, and one stray report is then forgotten", "[client_trust]")
{
    HeldJump held;
    REQUIRE(JudgeStep({100.f, 100.f, 0.f}, 1000, {900000.f, 100.f, 0.f}, 1033, false, held) == Step::kHold);
    REQUIRE(held.Held);
    // The next report is back by the actor, so it was a stray one.
    REQUIRE(JudgeStep({100.f, 100.f, 0.f}, 1000, {120.f, 100.f, 0.f}, 1066, false, held) == Step::kTake);
    REQUIRE_FALSE(held.Held);
}

TEST_CASE("a jump that stays is a teleport and is taken with the next report", "[client_trust]")
{
    HeldJump held;
    const Position cFrom{100.f, 100.f, 0.f};
    REQUIRE(JudgeStep(cFrom, 1000, {50000.f, 100.f, 0.f}, 1033, false, held) == Step::kHold);
    REQUIRE(JudgeStep(cFrom, 1000, {50010.f, 100.f, 0.f}, 1066, false, held) == Step::kTake);
    REQUIRE_FALSE(held.Held);
}

TEST_CASE("a jump followed by another one elsewhere is held again", "[client_trust]")
{
    HeldJump held;
    const Position cFrom{100.f, 100.f, 0.f};
    REQUIRE(JudgeStep(cFrom, 1000, {50000.f, 100.f, 0.f}, 1033, false, held) == Step::kHold);
    REQUIRE(JudgeStep(cFrom, 1000, {-70000.f, 100.f, 0.f}, 1066, false, held) == Step::kHold);
    REQUIRE(held.At.X == -70000.f);
    REQUIRE(JudgeStep(cFrom, 1000, {-70020.f, 100.f, 0.f}, 1099, false, held) == Step::kTake);
}

TEST_CASE("a new cell is a load or a door and is taken at once", "[client_trust]")
{
    HeldJump held{true, {1.f, 1.f, 1.f}};
    REQUIRE(JudgeStep({0.f, 0.f, 0.f}, 1000, {300000.f, 0.f, 0.f}, 1033, true, held) == Step::kTake);
    REQUIRE_FALSE(held.Held);
}

TEST_CASE("two reports in one tick make no speed of their own", "[client_trust]")
{
    HeldJump held;
    // The same tick, and a tick that went back: 50 ms is counted, so a 200 unit step is a jitter and not a jump.
    REQUIRE(JudgeStep({0.f, 0.f, 0.f}, 1000, {200.f, 0.f, 0.f}, 1000, false, held) == Step::kTake);
    REQUIRE(JudgeStep({0.f, 0.f, 0.f}, 1000, {200.f, 0.f, 0.f}, 900, false, held) == Step::kTake);
    REQUIRE(JudgeStep({0.f, 0.f, 0.f}, 1000, {kStepSlack + kMaxSpeed, 0.f, 0.f}, 1000, false, held) == Step::kHold);
}

TEST_CASE("a correction is sent at most once in a while to each sender", "[client_trust]")
{
    CorrectionWindow window;
    const auto cStart = std::chrono::steady_clock::now();
    REQUIRE(window.Due(cStart));
    REQUIRE_FALSE(window.Due(cStart + std::chrono::seconds(1)));
    REQUIRE_FALSE(window.Due(cStart + kCorrectionEvery - std::chrono::milliseconds(1)));
    REQUIRE(window.Due(cStart + kCorrectionEvery));
    REQUIRE_FALSE(window.Due(cStart + kCorrectionEvery + std::chrono::seconds(1)));
}

TEST_CASE("a place and a turn count only when they are numbers inside the world", "[client_trust]")
{
    const float cNaN = std::numeric_limits<float>::quiet_NaN();
    const float cInf = std::numeric_limits<float>::infinity();
    REQUIRE(IsUsableTransform(100.f, -2000.f, 35.f, 0.f, 3.14f, -1.f));
    REQUIRE_FALSE(IsUsableTransform(cNaN, 0.f, 0.f, 0.f, 0.f, 0.f));
    REQUIRE_FALSE(IsUsableTransform(0.f, cInf, 0.f, 0.f, 0.f, 0.f));
    REQUIRE_FALSE(IsUsableTransform(0.f, 0.f, kMaxCoordinate * 2.f, 0.f, 0.f, 0.f));
    REQUIRE_FALSE(IsUsableTransform(0.f, 0.f, 0.f, cNaN, 0.f, 0.f));
    REQUIRE_FALSE(IsUsableTransform(0.f, 0.f, 0.f, 0.f, -cInf, 0.f));
    REQUIRE_FALSE(IsUsableTransform(0.f, 0.f, 0.f, 0.f, 0.f, cNaN));
}

TEST_CASE("every refusal has its own name", "[client_trust]")
{
    std::set<std::string> names;
    for (const Refusal refusal :
         {Refusal::kNone, Refusal::kNotFinite, Refusal::kUnknownActor, Refusal::kNotOwner, Refusal::kOutOfRange, Refusal::kPvpOff, Refusal::kTooLong, Refusal::kBadPosition})
        names.insert(RefusalName(refusal));
    REQUIRE(names.size() == 8);
    REQUIRE(names.count("?") == 0);
}
