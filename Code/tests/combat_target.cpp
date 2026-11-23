#include <catch2/catch.hpp>

#include <CombatTargets.h>

#include <limits>

using namespace CombatTargets;

namespace
{
constexpr uint64_t kNever = std::numeric_limits<uint64_t>::max();

Word WordOf(const uint32_t aNpcId, const uint32_t aTargetId, const uint32_t aEpoch = 3) noexcept
{
    return {aNpcId, aTargetId, aEpoch};
}
} // namespace

TEST_CASE("The owner sends a fight when its target changes, and every fight at a heartbeat", "[combat_target]")
{
    Outbox outbox;
    uint64_t now = 1000;

    // The first round is a heartbeat: every fight goes, and an NPC that fights nobody says nothing.
    outbox.Put(7, 20, 3);
    outbox.Put(8, 21, 3);
    outbox.Put(9, 0, 3);
    CHECK(outbox.Take(now).size() == 2);

    // Nothing changed: nothing goes.
    now += kReadEveryMs;
    outbox.Put(7, 20, 3);
    outbox.Put(8, 21, 3);
    outbox.Put(9, 0, 3);
    CHECK(outbox.Take(now).empty());

    // One turned to another enemy: only it goes.
    now += kReadEveryMs;
    outbox.Put(7, 22, 3);
    outbox.Put(8, 21, 3);
    outbox.Put(9, 0, 3);
    auto words = outbox.Take(now);
    REQUIRE(words.size() == 1);
    CHECK(words[0].NpcId == 7);
    CHECK(words[0].TargetId == 22);
    CHECK(words[0].OwnershipEpoch == 3);

    // At the heartbeat, every fight goes again, and still nothing for the NPC that fights nobody.
    now = 1000 + kHeartbeatMs;
    outbox.Put(7, 22, 3);
    outbox.Put(8, 21, 3);
    outbox.Put(9, 0, 3);
    words = outbox.Take(now);
    REQUIRE(words.size() == 2);
    for (const Word& cWord : words)
        CHECK(cWord.TargetId != 0);
}

TEST_CASE("A fight that ends sends one 0, and then nothing", "[combat_target]")
{
    Outbox outbox;
    outbox.Put(7, 20, 3);
    CHECK(outbox.Take(1000).size() == 1);

    outbox.Put(7, 0, 3);
    auto words = outbox.Take(1250);
    REQUIRE(words.size() == 1);
    CHECK(words[0].NpcId == 7);
    CHECK(words[0].TargetId == 0);
    CHECK(words[0].OwnershipEpoch == 3);

    // Dropped: no second 0, and none at the heartbeat either.
    outbox.Put(7, 0, 3);
    CHECK(outbox.Take(1500).empty());
    outbox.Put(7, 0, 3);
    CHECK(outbox.Take(1000 + kHeartbeatMs).empty());

    // A new fight is new again.
    outbox.Put(7, 21, 3);
    CHECK(outbox.Take(1000 + kHeartbeatMs + kReadEveryMs).size() == 1);
}

TEST_CASE("An NPC no longer owned is forgotten without a word", "[combat_target]")
{
    Outbox outbox;
    outbox.Put(7, 20, 3);
    CHECK(outbox.Take(1000).size() == 1);

    // Handed on: not put in the round, so no 0 goes; its new owner speaks for it now.
    CHECK(outbox.Take(1250).empty());
    CHECK(outbox.Take(1000 + kHeartbeatMs).empty());

    // Gained back while fighting nobody: still nothing.
    outbox.Put(7, 0, 4);
    CHECK(outbox.Take(1000 + kHeartbeatMs + kReadEveryMs).empty());
}

TEST_CASE("A new epoch for the same fight is sent at once", "[combat_target]")
{
    Outbox outbox;
    outbox.Put(7, 20, 3);
    CHECK(outbox.Take(1000).size() == 1);

    outbox.Put(7, 20, 4);
    auto words = outbox.Take(1250);
    REQUIRE(words.size() == 1);
    CHECK(words[0].OwnershipEpoch == 4);
}

TEST_CASE("The server takes a word only from the NPC's owner at the epoch it holds", "[combat_target]")
{
    const Word cWord = WordOf(7, 20, 3);

    CHECK(ServerTakes(true, false, true, 3, cWord, true, true, kNever) == ServerVerdict::kRelay);
    // Another player's game, which does not run it.
    CHECK(ServerTakes(true, false, false, 3, cWord, true, true, kNever) == ServerVerdict::kEpochDrop);
    // Its owner, at an epoch from before a hand-off.
    CHECK(ServerTakes(true, false, true, 4, cWord, true, true, kNever) == ServerVerdict::kEpochDrop);
    // Epoch 0 is never valid, even where the server holds 0.
    CHECK(ServerTakes(true, false, true, 0, WordOf(7, 20, 0), true, true, kNever) == ServerVerdict::kEpochDrop);
    // A player's character, or no character at all.
    CHECK(ServerTakes(true, true, true, 3, cWord, true, true, kNever) == ServerVerdict::kSkip);
    CHECK(ServerTakes(false, false, true, 3, cWord, true, true, kNever) == ServerVerdict::kSkip);
}

TEST_CASE("The server takes a target that is nobody or another character", "[combat_target]")
{
    // Nobody: the fight ended.
    CHECK(ServerTakes(true, false, true, 3, WordOf(7, 0), false, true, kNever) == ServerVerdict::kRelay);
    // Itself.
    CHECK(ServerTakes(true, false, true, 3, WordOf(7, 7), true, true, kNever) == ServerVerdict::kSkip);
    // Not a character, or gone.
    CHECK(ServerTakes(true, false, true, 3, WordOf(7, 20), false, true, kNever) == ServerVerdict::kSkip);
}

TEST_CASE("The server passes on a changed target at once, and the same one at most every 1.5 s", "[combat_target]")
{
    const Word cWord = WordOf(7, 20, 3);

    CHECK(ServerTakes(true, false, true, 3, cWord, true, true, 0) == ServerVerdict::kRelay);
    CHECK(ServerTakes(true, false, true, 3, cWord, true, false, 0) == ServerVerdict::kStore);
    CHECK(ServerTakes(true, false, true, 3, cWord, true, false, kRelayEveryMs - 1) == ServerVerdict::kStore);
    CHECK(ServerTakes(true, false, true, 3, cWord, true, false, kRelayEveryMs) == ServerVerdict::kRelay);
    // The heartbeat gets through, and nothing faster.
    CHECK(kRelayEveryMs < kHeartbeatMs);
    CHECK(kRelayEveryMs > kReadEveryMs);
}

TEST_CASE("A copy takes a word only for a copy at the word's epoch", "[combat_target]")
{
    CHECK(CopyTakes(true, true, 3, 3) == CopyVerdict::kTake);
    CHECK(CopyTakes(true, true, 4, 3) == CopyVerdict::kEpochMiss);
    CHECK(CopyTakes(true, true, 0, 0) == CopyVerdict::kEpochMiss);
    // This game runs it: its own AI decides whom it fights.
    CHECK(CopyTakes(true, false, 0, 3) == CopyVerdict::kOwn);
    CHECK(CopyTakes(false, false, 0, 3) == CopyVerdict::kUnknown);
}

TEST_CASE("A copy's word goes after 5 s without another", "[combat_target]")
{
    CHECK_FALSE(CopyExpired(1000, 1000));
    CHECK_FALSE(CopyExpired(1000, 1000 + kHeartbeatMs));
    CHECK_FALSE(CopyExpired(1000, 1000 + kExpireMs));
    CHECK(CopyExpired(1000, 1000 + kExpireMs + 1));
    // Two heartbeats can be lost before it goes.
    CHECK(kExpireMs > 2 * kHeartbeatMs);
}

TEST_CASE("A copy looks at its owner's target, and is pointed at it only with a controller of its own", "[combat_target]")
{
    CHECK(CopyAction(false, true, true) == CopyStep::kLookAt);
    CHECK(CopyAction(true, true, true) == CopyStep::kLookAtAndPoint);
    // Not here: kept for the next word, whatever the controller.
    CHECK(CopyAction(false, false, false) == CopyStep::kKeep);
    CHECK(CopyAction(true, false, false) == CopyStep::kKeep);
    // Dead, disabled or not drawn: let go of.
    CHECK(CopyAction(false, true, false) == CopyStep::kClear);
    CHECK(CopyAction(true, true, false) == CopyStep::kClear);
}

TEST_CASE("The game starts no fight of its own for an NPC copy", "[combat_target]")
{
    CHECK(MayStartOwnCombat(false, false));
    CHECK_FALSE(MayStartOwnCombat(true, false));
    // A player's copy is left to the game.
    CHECK(MayStartOwnCombat(true, true));
}
