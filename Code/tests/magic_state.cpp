#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <catch2/catch.hpp>

#include <DecodeGuard.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/RequestMagicState.h>
#include <Messages/NotifyMagicState.h>

#include <MagicStates.h>
#include <Games/Magic/SharedSelfEffects.h>

#include <cmath>
#include <limits>

using namespace TiltedPhoques;

namespace
{
// Flame Cloak (Skyrim.esm 0x3AE9F) with its effect, 20 s into its 60.
MagicStateEffect Cloak(const float aElapsed = 20.f) noexcept
{
    return {GameId{0, 0x3AE9F}, GameId{0, 0x3AEA0}, 8.f, aElapsed, 60.f};
}

MagicStateEffect Oakflesh() noexcept
{
    return {GameId{0, 0x5AD5C}, GameId{0, 0x5AD5D}, 40.f, 3.f, 60.f};
}

MagicState Full() noexcept
{
    MagicState state;
    for (uint32_t i = 0; i < MagicState::kMaxEffects; ++i)
        state.Effects.push_back({GameId{0xFE000000u + i, 0x800 + i}, GameId{i % 3, 0xFFFFF000u - i}, static_cast<float>(i) * 1.5f, static_cast<float>(i), 100.f + i});
    state.LeftHand = {GameId{0, 0x12FCD}, true, GameId{0, 0x12FD0}};
    state.RightHand = {GameId{0xFFFFFFFF, 0xFFFFFFFE}, false, GameId{0xFFFFFFFF, 0xFFFFFFFF}};
    return state;
}

template <class T, class TFactory> void RoundTrip(const T& acMessage)
{
    Buffer buff(10000);
    Buffer::Writer writer(&buff);
    acMessage.Serialize(writer);
    Buffer::Reader reader(&buff);
    auto pMessage = TFactory{}.Extract(reader);
    REQUIRE(pMessage);
    REQUIRE(pMessage->GetOpcode() == T::Opcode);
    REQUIRE(*CastUnique<T>(std::move(pMessage)) == acMessage);
}
} // namespace

TEST_CASE("RequestMagicState and NotifyMagicState round trip", "[magicstate]")
{
    MagicState one;
    one.Effects.push_back(Cloak());
    MagicState casting;
    casting.RightHand = {GameId{0, 0x12FCD}, false};
    // Firebolt charging in the left hand.
    MagicState charging;
    charging.LeftHand.ChargeId = GameId{0, 0x12FD0};
    const MagicState cCases[] = {{}, one, casting, charging, Full()};

    for (const auto& cState : cCases)
    {
        RequestMagicState request;
        request.CharacterId = 0x80001234;
        request.State = cState;
        RoundTrip<RequestMagicState, ClientMessageFactory>(request);

        NotifyMagicState notify;
        notify.CharacterId = 0xFFFFFFFF;
        notify.State = cState;
        RoundTrip<NotifyMagicState, ServerMessageFactory>(notify);
    }
}

TEST_CASE("A magic state with more effects than a message holds is dropped whole", "[magicstate]")
{
    MagicState tooMany = Full();
    tooMany.Effects.push_back(Cloak());

    {
        Buffer buff(10000);
        Buffer::Writer writer(&buff);
        RequestMagicState request;
        request.State = tooMany;
        request.Serialize(writer);
        Buffer::Reader reader(&buff);
        REQUIRE(!ClientMessageFactory{}.Extract(reader));
        REQUIRE(DecodeGuard::t_failed);
        DecodeGuard::t_failed = false;
    }
    {
        Buffer buff(10000);
        Buffer::Writer writer(&buff);
        NotifyMagicState notify;
        notify.State = tooMany;
        notify.Serialize(writer);
        Buffer::Reader reader(&buff);
        REQUIRE(!ServerMessageFactory{}.Extract(reader));
        REQUIRE(DecodeGuard::t_failed);
        DecodeGuard::t_failed = false;
    }
}

TEST_CASE("The server keeps only effects with real times from a state", "[magicstate]")
{
    using namespace MagicStates;
    const float cNan = std::numeric_limits<float>::quiet_NaN();
    const float cInf = std::numeric_limits<float>::infinity();

    MagicState state;
    state.Effects.push_back(Cloak());
    state.Effects.push_back({GameId{}, GameId{0, 1}, 1.f, 0.f, 10.f});        // no spell
    state.Effects.push_back({GameId{0, 1}, GameId{}, 1.f, 0.f, 10.f});        // no effect
    state.Effects.push_back({GameId{0, 1}, GameId{0, 2}, cNan, 0.f, 10.f});   // magnitude not a number
    state.Effects.push_back({GameId{0, 1}, GameId{0, 2}, 1.f, cInf, 10.f});   // elapsed forever
    state.Effects.push_back({GameId{0, 1}, GameId{0, 2}, 1.f, -1.f, 10.f});   // before it began
    state.Effects.push_back({GameId{0, 1}, GameId{0, 2}, 1.f, 10.f, 10.f});   // already over
    state.Effects.push_back({GameId{0, 1}, GameId{0, 2}, 1.f, 0.f, 0.f});     // no duration: an ability
    state.Effects.push_back({GameId{0, 1}, GameId{0, 2}, 1.f, 0.f, 90000.f}); // longer than a day
    state.Effects.push_back(Oakflesh());
    state.LeftHand = {GameId{0, 0x12FCD}, true};

    const MagicState cClean = Sanitized(state);
    REQUIRE(cClean.Effects.size() == 2);
    CHECK(cClean.Effects[0] == Cloak());
    CHECK(cClean.Effects[1] == Oakflesh());
    CHECK(cClean.LeftHand == state.LeftHand);
}

TEST_CASE("A kept state is aged for a player who sees it later", "[magicstate]")
{
    using namespace MagicStates;

    MagicState state;
    state.Effects.push_back(Cloak(20.f));
    state.Effects.push_back(Oakflesh());
    state.RightHand = {GameId{0, 0x12FCD}, false};

    // 30 s later the cloak has 10 s left; 50 s later it is gone, Oakflesh too at 57; the held spell stays.
    const MagicState cAt30 = Aged(state, 30.f);
    REQUIRE(cAt30.Effects.size() == 2);
    CHECK(cAt30.Effects[0].Elapsed == 50.f);
    CHECK(cAt30.Effects[1].Elapsed == 33.f);
    CHECK(Aged(state, 45.f).Effects.size() == 1);
    CHECK(Aged(state, 57.f).Effects.empty());
    CHECK(Aged(state, 57.f).RightHand == state.RightHand);
    CHECK_FALSE(Aged(state, 57.f).Empty());

    // A clock gone back or not a number ages nothing.
    CHECK(Aged(state, -5.f) == state);
    CHECK(Aged(state, std::numeric_limits<float>::quiet_NaN()) == state);
}

TEST_CASE("The owner sends its state when it changes, and again every 5 s while it has something", "[magicstate]")
{
    using namespace MagicStates;

    MagicState cloak;
    cloak.Effects.push_back(Cloak(20.f));
    MagicState later;
    later.Effects.push_back(Cloak(21.f));
    MagicState otherHand = cloak;
    otherHand.LeftHand = {GameId{0, 0x12FCD}, false};
    MagicState two = cloak;
    two.Effects.push_back(Oakflesh());

    // Time alone is no change; another effect or a cast is.
    CHECK(SameShape(cloak, later));
    CHECK_FALSE(SameShape(cloak, otherHand));
    CHECK_FALSE(SameShape(cloak, two));
    CHECK_FALSE(SameShape(cloak, MagicState{}));

    // Nothing said yet: only something worth saying.
    CHECK_FALSE(ShouldSend(false, true, true, 0));
    CHECK(ShouldSend(false, false, false, 0));
    // A change goes at once, also to nothing (the cloak ended).
    CHECK(ShouldSend(true, false, true, 0));
    CHECK(ShouldSend(true, false, false, 10));
    // No change: a heartbeat while there is something.
    CHECK_FALSE(ShouldSend(true, true, false, kHeartbeatMs - 1));
    CHECK(ShouldSend(true, true, false, kHeartbeatMs));
    CHECK_FALSE(ShouldSend(true, true, true, kHeartbeatMs * 10));
}

TEST_CASE("A copy gets what its owner has and loses what its owner no longer has", "[magicstate]")
{
    using namespace MagicStates;

    const Key cCloak{0x3AE9F, 0x3AEA0};
    const Key cOak{0x5AD5C, 0x5AD5D};
    const Key cHealing{0x12FCC, 0x12FCE};

    // Arriving late: the cloak is put on, Oakflesh is already there from the live effect.
    {
        const Plan cPlan = PlanEffects({cCloak, cOak}, {40.f, 57.f}, {{cOak, 3.f, true}}, true);
        REQUIRE(cPlan.Add.size() == 1);
        CHECK(cPlan.Add[0] == 0);
        CHECK(cPlan.End.empty());
    }

    // The owner's cloak ended early (a recast, a dispel): the copy's ends. Someone else's heal on the copy is not the state's.
    {
        const Plan cPlan = PlanEffects({cOak}, {57.f}, {{cCloak, 30.f, true}, {cOak, 3.f, true}, {cHealing, 30.f, false}}, true);
        CHECK(cPlan.Add.empty());
        REQUIRE(cPlan.End.size() == 1);
        CHECK(cPlan.End[0] == 0);
    }

    // A cloak the live effect put on just now may be newer than the state: it stays.
    CHECK(PlanEffects({}, {}, {{cCloak, kNewOnCopy - 0.1f, true}}, true).End.empty());
    // Only a new state ends anything.
    CHECK(PlanEffects({}, {}, {{cCloak, 30.f, true}}, false).End.empty());
    // Too little left, or an effect this game does not have (no left): not put on.
    CHECK(PlanEffects({cCloak}, {kShortestLeft - 0.1f}, {}, true).Add.empty());
    CHECK(PlanEffects({cCloak}, {0.f}, {}, true).Add.empty());
    CHECK(PlanEffects({cCloak}, {}, {}, true).Add.empty());
}

TEST_CASE("A copy's hand casts what its owner's hand casts", "[magicstate]")
{
    using namespace MagicStates;

    const uint64_t cFlames = 0x12FCD;
    const uint64_t cWard = 0x211EE;

    CHECK_FALSE(PlanCast(cFlames, cFlames).Interrupt);
    CHECK_FALSE(PlanCast(cFlames, cFlames).Cast);
    CHECK_FALSE(PlanCast(0, 0).Cast);

    // Seen late: cast it. Its end was missed: stop it. Another spell now: stop and cast.
    CHECK(PlanCast(0, cFlames).Cast);
    CHECK_FALSE(PlanCast(0, cFlames).Interrupt);
    CHECK(PlanCast(cFlames, 0).Interrupt);
    CHECK_FALSE(PlanCast(cFlames, 0).Cast);
    CHECK(PlanCast(cFlames, cWard).Interrupt);
    CHECK(PlanCast(cFlames, cWard).Cast);
}

TEST_CASE("Only a player's own lasting, visible effects are shown on their copy", "[magicstate]")
{
    using namespace SharedSelfEffects;
    using namespace EffectArchetypes;

    const Facts cCloak{true, 20.f, 60.f, 0, kCloak, true, false, false};
    CHECK(IsShared(cCloak));

    // Oakflesh's shader, Candlelight's light, Invisibility's own look.
    CHECK(HasVisual(true, false, false, kValueModifier));
    CHECK(HasVisual(false, false, true, kLight));
    CHECK(HasVisual(false, false, false, kInvisibility));
    CHECK(HasVisual(false, false, false, kEtherealize));
    // Muffle: nothing to see.
    CHECK_FALSE(HasVisual(false, false, false, kValueModifier));

    auto facts = cCloak;
    facts.FromSelf = false; // another player's or an NPC's spell on them
    CHECK_FALSE(IsShared(facts));
    facts = cCloak;
    facts.Visual = false;
    CHECK_FALSE(IsShared(facts));
    facts = cCloak;
    facts.Personal = true; // a shrine's or a stone's
    CHECK_FALSE(IsShared(facts));
    facts = cCloak;
    facts.LocalControl = true;
    CHECK_FALSE(IsShared(facts));
    facts = cCloak;
    facts.Duration = 0.f; // an ability, a worn enchantment, a disease
    facts.Elapsed = 0.f;
    CHECK_FALSE(IsShared(facts));
    facts = cCloak;
    facts.Elapsed = 60.f;
    CHECK_FALSE(IsShared(facts));
    facts = cCloak;
    facts.Flags = kInactive;
    CHECK_FALSE(IsShared(facts));
    facts = cCloak;
    facts.Flags = kDispelled;
    CHECK_FALSE(IsShared(facts));

    // Synced on their own (summons, bound weapons, beast forms, Slow Time) or this player's own view (Vampire's Sight, Detect Life).
    for (const ArchetypeID cArchetype : {kSummonCreature, kBoundWeapon, kWerewolf, kVampireLord, kSlowTime, kNightEye, kDetectLife, kTelekinesis, kReanimate})
    {
        facts = cCloak;
        facts.Archetype = cArchetype;
        CHECK_FALSE(IsShared(facts));
    }
    for (const ArchetypeID cArchetype : {kValueModifier, kDualValueModifier, kPeakValueModifier, kInvisibility, kLight, kEtherealize, kScript})
    {
        facts = cCloak;
        facts.Archetype = cArchetype;
        CHECK(IsShared(facts));
    }
}

TEST_CASE("Standing stones' gifts stay with the player who used the stone", "[magicstate]")
{
    using namespace SharedSelfEffects;

    // The Lord Stone and the Earth Stone (Solstheim), whatever the plugin name's case.
    CHECK(IsStandingStoneSpell("Skyrim.esm", 0x0E5F58));
    CHECK(IsStandingStoneSpell("skyrim.ESM", 0x0E5F58));
    CHECK(IsStandingStoneSpell("Dragonborn.esm", 0x01DF9F));
    // The Shadow Stone's power is cast by the player later, like any power.
    CHECK_FALSE(IsStandingStoneSpell("Skyrim.esm", 0x0E732A));
    // The same id in another plugin.
    CHECK_FALSE(IsStandingStoneSpell("Dawnguard.esm", 0x0E5F58));
    CHECK_FALSE(IsStandingStoneSpell("Skyrim.es", 0x0E5F58));
    // Shrines are known by their cure.
    CHECK(kShrineCureEffect.BaseId == 0x0FBFF5);
}

TEST_CASE("With PvP off another player's hostile effect does not land on this player", "[magicstate]")
{
    using namespace MagicSystem;

    constexpr uint32_t cHostile = 1;
    constexpr uint32_t cDetrimental = 4;
    // Paralyze, Fear, a damage effect.
    CHECK_FALSE(PlayerEffectTaken(false, cHostile));
    CHECK_FALSE(PlayerEffectTaken(false, cDetrimental));
    CHECK_FALSE(PlayerEffectTaken(false, cHostile | cDetrimental | 0x200));
    // Healing Hands, a buff.
    CHECK(PlayerEffectTaken(false, 0));
    CHECK(PlayerEffectTaken(false, 0x200));
    // PvP on: the caster's game decided it.
    CHECK(PlayerEffectTaken(true, cHostile | cDetrimental));
}

TEST_CASE("A hand charging a spell is part of the state, and a change of it is sent at once", "[magicstate]")
{
    using namespace MagicStates;

    MagicState charging;
    charging.RightHand.ChargeId = GameId{0, 0x12FD0};
    CHECK_FALSE(charging.Empty());
    CHECK_FALSE(SameShape(charging, MagicState{}));
    // Starting and ending a charge are both sent without waiting for the heartbeat.
    CHECK(ShouldSend(false, false, charging.Empty(), 0));
    CHECK(ShouldSend(true, SameShape(MagicState{}, charging), MagicState{}.Empty(), 0));

    MagicState other = charging;
    other.RightHand.ChargeId = GameId{0, 0x1C789};
    CHECK_FALSE(SameShape(charging, other));

    // The server keeps it, and a player who arrives later is told.
    CHECK(Sanitized(charging).RightHand.ChargeId == charging.RightHand.ChargeId);
    CHECK(Aged(charging, 3.f).RightHand.ChargeId == charging.RightHand.ChargeId);
}

TEST_CASE("Only a fire-and-forget spell the hand is charging shows as a charge", "[magicstate]")
{
    using namespace MagicStates;

    // Firebolt or a summon from the start of the cast to held ready (MagicCaster::State 1 to 5).
    for (uint32_t state = 1; state <= 5; ++state)
        CHECK(IsCharging(state, true, false, false));
    // Idle with the spell staged, released and casting, and the interrupt states of a cancel.
    for (const uint32_t cState : {0u, 6u, 7u, 8u, 9u, 10u, 0xFFFFFFFFu})
        CHECK_FALSE(IsCharging(cState, true, false, false));
    // A dead owner charges nothing.
    CHECK_FALSE(IsCharging(5, true, false, true));
    // Flames and wards are sent as their cast; Invisibility as well.
    CHECK_FALSE(IsCharging(5, false, true, false));
    CHECK_FALSE(IsCharging(5, true, true, false));
    CHECK_FALSE(IsCharging(5, false, false, false));
}

TEST_CASE("A charge that ends, is cancelled or dies is cleared on the server, so no one arriving later sees it", "[magicstate]")
{
    using namespace MagicStates;

    MagicState charging;
    charging.LeftHand.ChargeId = GameId{0, 0x12FD0};
    MagicState kept = Sanitized(charging);
    REQUIRE(Aged(kept, 1.f).LeftHand.ChargeId == charging.LeftHand.ChargeId);

    // Released, cancelled or dead: the hand reads as not charging, so the owner's next state has no charge.
    for (const auto& [cState, cDead] : {std::pair{0u, false}, std::pair{6u, false}, std::pair{9u, false}, std::pair{3u, true}})
    {
        const bool cCharging = IsCharging(cState, true, false, cDead);
        CHECK_FALSE(cCharging);
        MagicState after;
        if (cCharging)
            after.LeftHand.ChargeId = charging.LeftHand.ChargeId;
        // It is sent at once, the server keeps it in place of the old one, and a late joiner gets nothing.
        CHECK(ShouldSend(true, SameShape(after, kept), after.Empty(), 0));
        const MagicState cKeptNow = Sanitized(after);
        CHECK_FALSE(cKeptNow.LeftHand.ChargeId);
        CHECK(Aged(cKeptNow, 10.f).Empty());
    }

    // A disconnect resets the owner to never having sent; an empty state is then not sent at all (the server drops the character).
    CHECK_FALSE(ShouldSend(false, false, MagicState{}.Empty(), 0));
}

TEST_CASE("A damaged magic state from a peer decodes without a crash and never invents a charge", "[magicstate]")
{
    MagicState charging;
    charging.RightHand.ChargeId = GameId{0, 0x12FD0};
    NotifyMagicState notify;
    notify.CharacterId = 7;
    notify.State = charging;

    Buffer whole(1000);
    Buffer::Writer writer(&whole);
    notify.Serialize(writer);
    const size_t cSize = writer.GetBytePosition();

    for (size_t size = 1; size < cSize; ++size)
    {
        Buffer cut(whole.GetData(), size);
        Buffer::Reader reader(&cut);
        auto pMessage = ServerMessageFactory{}.Extract(reader);
        DecodeGuard::t_failed = false;
        if (!pMessage)
            continue;
        const auto& cDecoded = CastUnique<NotifyMagicState>(std::move(pMessage))->State;
        // What is cut away reads as nothing, never as another hand's charge.
        CHECK_FALSE(cDecoded.LeftHand.ChargeId);
    }

    // An id no game has round trips as data; the game that gets it finds no spell and shows nothing.
    MagicState unknown;
    unknown.LeftHand.ChargeId = GameId{0xFFFFFFFF, 0xFFFFFFFF};
    notify.State = unknown;
    RoundTrip<NotifyMagicState, ServerMessageFactory>(notify);
}

TEST_CASE("A copy's hand glows with what its owner's hand charges, and again on a new body", "[magicstate]")
{
    using namespace MagicStates;

    constexpr uint64_t cFirebolt = 0x12FD0;
    constexpr uint64_t cConjure = 0x204C3;

    CHECK_FALSE(PlanCharge(0, 0, false).Play);
    CHECK_FALSE(PlanCharge(0, 0, true).Play);
    CHECK_FALSE(PlanCharge(0, 0, true).Stop);
    CHECK_FALSE(PlanCharge(cFirebolt, cFirebolt, false).Play);
    CHECK_FALSE(PlanCharge(cFirebolt, cFirebolt, false).Stop);

    CHECK(PlanCharge(0, cFirebolt, false).Play);
    CHECK_FALSE(PlanCharge(0, cFirebolt, false).Stop);
    CHECK(PlanCharge(cFirebolt, 0, false).Stop);
    CHECK_FALSE(PlanCharge(cFirebolt, 0, false).Play);
    CHECK(PlanCharge(cFirebolt, cConjure, false).Stop);
    CHECK(PlanCharge(cFirebolt, cConjure, false).Play);

    // The glow went with the old body: it is played again on the new one.
    CHECK(PlanCharge(cFirebolt, cFirebolt, true).Stop);
    CHECK(PlanCharge(cFirebolt, cFirebolt, true).Play);
}

TEST_CASE("A copy's glow is played again before it runs out while the charge lasts, and runs out by itself after", "[magicstate]")
{
    using namespace MagicStates;

    constexpr uint64_t cFirebolt = 0x12FD0;
    // Nothing of the game's effect is held: each glow ends by itself, so it is played again while the charge lasts.
    CHECK(RenewsGlow(cFirebolt, 0));
    CHECK(RenewsGlow(cFirebolt, kGlowRenewMs));
    CHECK_FALSE(RenewsGlow(cFirebolt, kGlowRenewMs + 1));
    // The charge ended: no more, and the last glow is gone within kGlowSeconds.
    CHECK_FALSE(RenewsGlow(0, 0));
    // Checked every frame: two frames of a slow game, so it never goes out in between.
    static_assert(kGlowRenewMs >= 2 * kSlowFrameMs);
    CHECK(kGlowSeconds * 1000.f > static_cast<float>(kGlowRenewMs));
    // The next glow and the last overlap a small part of each glow only (400 of every 600 ms before).
    CHECK(static_cast<float>(kGlowRenewMs) <= kGlowSeconds * 1000.f / 10.f);
}
