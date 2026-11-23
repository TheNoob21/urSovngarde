#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <HitReactions.h>
#include <Messages/RequestHitReaction.h>
#include <Messages/NotifyHitReaction.h>

#include <limits>
#include <string>

using namespace TiltedPhoques;
using namespace HitReactions;
using namespace std::chrono_literals;

namespace
{
template <class T> T RoundTrip(const T& acMessage)
{
    Buffer buffer(64);
    Buffer::Writer writer(&buffer);
    acMessage.SerializeRaw(writer);
    Buffer::Reader reader(&buffer);
    T back{};
    back.DeserializeRaw(reader);
    return back;
}
} // namespace

TEST_CASE("a hit reaction keeps the actor, kind and numbers", "[hitreactions]")
{
    for (const uint8_t kind : {uint8_t{0}, uint8_t{1}, uint8_t{2}})
        for (const float magnitude : {0.f, 0.35f, 1.f})
            for (const float direction : {0.f, 0.25f, 0.999f})
            {
                RequestHitReaction request{};
                request.Id = 0x1234567;
                request.Kind = kind;
                request.Magnitude = magnitude;
                request.Direction = direction;
                REQUIRE(RoundTrip(request) == request);

                NotifyHitReaction notify{};
                notify.Id = request.Id;
                notify.Kind = kind;
                notify.Magnitude = magnitude;
                notify.Direction = direction;
                REQUIRE(RoundTrip(notify) == notify);
            }
}

TEST_CASE("the game's three reaction actions map both ways", "[hitreactions]")
{
    for (const Kind kind : {Kind::kStagger, Kind::kRecoil, Kind::kLargeRecoil})
        CHECK(KindOfAction(ActionOf(kind)) == kind);
    CHECK(ActionOf(Kind::kStagger) == 0x138D2);
    CHECK(ActionOf(Kind::kRecoil) == 0x13AF5);
    CHECK(ActionOf(Kind::kLargeRecoil) == 0x13EC8);
    // An attack or a move is no reaction.
    CHECK_FALSE(KindOfAction(0x13005).has_value());
    CHECK_FALSE(KindOfAction(0).has_value());
}

TEST_CASE("only a power attack or bash staggers when the game did not", "[hitreactions]")
{
    CHECK_FALSE(FallbackMagnitude(0).has_value());
    CHECK_FALSE(FallbackMagnitude(kSneakAttack).has_value());
    CHECK(FallbackMagnitude(kPowerAttack) == 0.5f);
    CHECK(FallbackMagnitude(kBash) == 0.35f);
    CHECK(FallbackMagnitude(kPowerAttack | kBash) == 0.75f);
    CHECK(FallbackMagnitude(kPowerAttack | kSneakAttack) == 0.5f);
    // A blocked blow staggers nobody.
    CHECK_FALSE(FallbackMagnitude(kPowerAttack | kBlocked).has_value());
    CHECK_FALSE(FallbackMagnitude(kBash | kBlocked).has_value());
}

TEST_CASE("the direction is where the blow came from, clockwise from the face", "[hitreactions]")
{
    constexpr float cQuarter = 1.57079632679f;
    // Facing +y (heading 0).
    CHECK(DirectionFrom(0.f, 0.f, 0.f, 0.f, 100.f) == Approx(0.f).margin(1e-5));
    CHECK(DirectionFrom(0.f, 0.f, 0.f, 100.f, 0.f) == Approx(0.25f));
    CHECK(DirectionFrom(0.f, 0.f, 0.f, 0.f, -100.f) == Approx(0.5f));
    CHECK(DirectionFrom(0.f, 0.f, 0.f, -100.f, 0.f) == Approx(0.75f));
    // Facing +x (heading a quarter turn): a blow from +x is in front, from +y on the left.
    CHECK(DirectionFrom(10.f, 10.f, cQuarter, 110.f, 10.f) == Approx(0.f).margin(1e-5));
    CHECK(DirectionFrom(10.f, 10.f, cQuarter, 10.f, 110.f) == Approx(0.75f));
    // Always within a turn, whatever the heading says.
    for (const float heading : {-20.f, -3.f, 0.f, 7.f, 40.f})
    {
        const float cDirection = DirectionFrom(0.f, 0.f, heading, 3.f, -4.f);
        CHECK(cDirection >= 0.f);
        CHECK(cDirection < 1.f);
    }
}

TEST_CASE("a reaction that cannot be played is not let through", "[hitreactions]")
{
    float magnitude = 0.5f;
    float direction = 0.25f;
    REQUIRE(Sanitize(0, magnitude, direction));
    CHECK(magnitude == 0.5f);
    CHECK(direction == 0.25f);

    magnitude = 0.5f;
    CHECK_FALSE(Sanitize(3, magnitude, direction));

    magnitude = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(Sanitize(0, magnitude, direction));
    magnitude = 0.5f;
    direction = std::numeric_limits<float>::infinity();
    CHECK_FALSE(Sanitize(0, magnitude, direction));

    // Out of range is brought back in.
    magnitude = 7.f;
    direction = -0.25f;
    REQUIRE(Sanitize(1, magnitude, direction));
    CHECK(magnitude == 1.f);
    CHECK(direction == Approx(0.75f));
    magnitude = -1.f;
    direction = 1.f;
    REQUIRE(Sanitize(2, magnitude, direction));
    CHECK(magnitude == 0.f);
    CHECK(direction == 0.f);
}

TEST_CASE("each screen shows a reaction once, and only the owner's game plays it", "[hitreactions]")
{
    // Three games: the hitter, the NPC's owner and a third player. The server sends it to all but the hitter.
    struct Game
    {
        bool Sender;
        bool Owner;
    };
    const Game cHitter{true, false};
    const Game cOwner{false, true};
    const Game cThird{false, false};

    CHECK(RoleOf(cHitter.Sender, cHitter.Owner) == Role::kNothing);
    CHECK(RoleOf(cOwner.Sender, cOwner.Owner) == Role::kPlay);
    CHECK(RoleOf(cThird.Sender, cThird.Owner) == Role::kPresetFloats);

    // One game plays it as an action; that action's replay is the one play every copy gets.
    int plays = 0;
    for (const Game& acGame : {cHitter, cOwner, cThird})
        plays += RoleOf(acGame.Sender, acGame.Owner) == Role::kPlay ? 1 : 0;
    CHECK(plays == 1);
}

TEST_CASE("the owner plays a reaction only on an actor that can take it", "[hitreactions]")
{
    const auto cNow = std::chrono::steady_clock::now();
    const std::chrono::steady_clock::time_point cNever{};

    ActorState standing;
    CHECK(CanPlay(standing, Kind::kStagger, cNow, cNever) == Refusal::kNone);
    CHECK(CanPlay(standing, Kind::kRecoil, cNow, cNever) == Refusal::kNone);

    const auto cRefused = [&](void (*apSet)(ActorState&), const Refusal aRefusal)
    {
        ActorState state;
        apSet(state);
        CHECK(CanPlay(state, Kind::kStagger, cNow, cNever) == aRefusal);
    };
    cRefused([](ActorState& s) { s.IsPlayer = true; }, Refusal::kPlayer);
    cRefused([](ActorState& s) { s.DeadOrDying = true; }, Refusal::kDead);
    cRefused([](ActorState& s) { s.BleedingOut = true; }, Refusal::kBleedingOut);
    cRefused([](ActorState& s) { s.KnockedDown = true; }, Refusal::kKnockedDown);
    cRefused([](ActorState& s) { s.Flying = true; }, Refusal::kFlying);
    cRefused([](ActorState& s) { s.SittingOrMounted = true; }, Refusal::kSittingOrMounted);
    cRefused([](ActorState& s) { s.Swimming = true; }, Refusal::kSwimming);
    cRefused([](ActorState& s) { s.InPairedAnimation = true; }, Refusal::kPairedAnimation);
    cRefused([](ActorState& s) { s.NoStagger = true; }, Refusal::kNoStagger);
    cRefused([](ActorState& s) { s.Has3D = false; }, Refusal::kNo3D);

    // bNoStagger holds off a stagger, not a recoil.
    ActorState noStagger;
    noStagger.NoStagger = true;
    CHECK(CanPlay(noStagger, Kind::kRecoil, cNow, cNever) == Refusal::kNone);
}

TEST_CASE("quick blows cannot keep an actor staggered", "[hitreactions]")
{
    const auto cNow = std::chrono::steady_clock::now();
    ActorState standing;
    CHECK(CanPlay(standing, Kind::kStagger, cNow, cNow - 100ms) == Refusal::kTooSoon);
    CHECK(CanPlay(standing, Kind::kStagger, cNow, cNow - 599ms) == Refusal::kTooSoon);
    CHECK(CanPlay(standing, Kind::kStagger, cNow, cNow - 600ms) == Refusal::kNone);
    CHECK(CanPlay(standing, Kind::kRecoil, cNow, cNow - 399ms) == Refusal::kTooSoon);
    CHECK(CanPlay(standing, Kind::kRecoil, cNow, cNow - 400ms) == Refusal::kNone);
}

TEST_CASE("the server relays a reaction only for an NPC near the sender, which the sender does not run", "[hitreactions]")
{
    // Character, player, has an owner, sender is the owner, in range.
    CHECK(ServerTakes(true, false, true, false, true));
    CHECK_FALSE(ServerTakes(false, false, true, false, true));
    CHECK_FALSE(ServerTakes(true, true, true, false, true));
    CHECK_FALSE(ServerTakes(true, false, false, false, true));
    CHECK_FALSE(ServerTakes(true, false, true, true, true));
    CHECK_FALSE(ServerTakes(true, false, true, false, false));
}

TEST_CASE("the server takes four reactions a second from one player for one actor", "[hitreactions]")
{
    const auto cStart = std::chrono::steady_clock::now();
    RateCount count;
    for (int i = 0; i < 4; ++i)
        CHECK(count.Take(cStart + std::chrono::milliseconds(i * 10)));
    CHECK_FALSE(count.Take(cStart + 50ms));
    CHECK_FALSE(count.Take(cStart + 999ms));
    // A new second starts over.
    CHECK(count.Take(cStart + 1000ms));
}

TEST_CASE("a copy's own raw stagger is held back for its owner", "[hitreactions]")
{
    RawGraphEvent event;
    event.IsCopy = true;
    event.IsStagger = true;
    CHECK(HoldsRawStagger(event));

    // The owner's stagger, replayed here: inside the replay, or its graph event coming just after it.
    RawGraphEvent replay = event;
    replay.PlayingAction = true;
    CHECK_FALSE(HoldsRawStagger(replay));
    replay = event;
    replay.ReplayedReaction = true;
    CHECK_FALSE(HoldsRawStagger(replay));

    // This game's own actors stagger as the game says, and another player's character is not an NPC copy.
    RawGraphEvent own = event;
    own.IsCopy = false;
    CHECK_FALSE(HoldsRawStagger(own));
    RawGraphEvent player = event;
    player.IsPlayer = true;
    CHECK_FALSE(HoldsRawStagger(player));

    // Only the stagger: other graph events on a copy go through.
    RawGraphEvent other = event;
    other.IsStagger = false;
    CHECK_FALSE(HoldsRawStagger(other));
}

TEST_CASE("a copy's replayed reaction lets its stagger through for half a second", "[hitreactions]")
{
    const auto cStart = std::chrono::steady_clock::now();
    ReplayedReactions replayed;
    CHECK_FALSE(replayed.Recently(0x1234, cStart));

    replayed.Note(0x1234, cStart);
    CHECK(replayed.Recently(0x1234, cStart));
    CHECK(replayed.Recently(0x1234, cStart + 500ms));
    CHECK_FALSE(replayed.Recently(0x1234, cStart + 501ms));
    CHECK_FALSE(replayed.Recently(0x5678, cStart));
    // A form id of 0 is no copy, even in an empty slot.
    CHECK_FALSE(replayed.Recently(0, cStart));

    // A new replay of the same copy starts the window again.
    replayed.Note(0x1234, cStart + 400ms);
    CHECK(replayed.Recently(0x1234, cStart + 800ms));

    // Many copies at once: the oldest give way, the latest are kept.
    for (uint32_t i = 1; i <= ReplayedReactions::kKept + 10; ++i)
        replayed.Note(0x10000 + i, cStart + 900ms);
    CHECK(replayed.Recently(0x10000 + ReplayedReactions::kKept + 10, cStart + 900ms));
    CHECK_FALSE(replayed.Recently(0x10001, cStart + 900ms));
}

TEST_CASE("the owner does not start a reaction over one already playing", "[hitreactions]")
{
    const auto cNow = std::chrono::steady_clock::now();
    const std::chrono::steady_clock::time_point cNever{};

    ActorState staggering;
    staggering.Staggering = true;
    CHECK(CanPlay(staggering, Kind::kStagger, cNow, cNever) == Refusal::kAlreadyReacting);
    CHECK(CanPlay(staggering, Kind::kRecoil, cNow, cNever) == Refusal::kAlreadyReacting);
    CHECK(CanPlay(staggering, Kind::kLargeRecoil, cNow, cNever) == Refusal::kAlreadyReacting);

    // A recoil does not hold off a stagger, which cuts it short in the game too; it holds off another recoil.
    ActorState recoiling;
    recoiling.Recoiling = true;
    CHECK(CanPlay(recoiling, Kind::kStagger, cNow, cNever) == Refusal::kNone);
    CHECK(CanPlay(recoiling, Kind::kRecoil, cNow, cNever) == Refusal::kAlreadyReacting);
    CHECK(CanPlay(recoiling, Kind::kLargeRecoil, cNow, cNever) == Refusal::kAlreadyReacting);

    // Long after this game's own last reaction, the actor's graph still decides: another player's blow can have started one meanwhile.
    CHECK(CanPlay(staggering, Kind::kStagger, cNow, cNow - 10s) == Refusal::kAlreadyReacting);
    CHECK(std::string(RefusalName(Refusal::kAlreadyReacting)) == "already staggering or recoiling");
}
