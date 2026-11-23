#include <catch2/catch.hpp>

#include <NpcHeadTrack.h>

#include <cmath>
#include <limits>

using namespace NpcHeadTrack;

namespace
{
constexpr uint64_t kNever = std::numeric_limits<uint64_t>::max();

constexpr Point kAtPlayer{1200.f, -340.f, 8100.f};
constexpr Look kLooking{kTracking, kAtPlayer};
constexpr Look kNotLooking{};

Word WordOf(const uint32_t aNpcId, const Look& acLook, const uint32_t aEpoch = 3) noexcept
{
    return {aNpcId, acLook, aEpoch};
}

Look MovedBy(const float aX) noexcept
{
    return {kTracking, {kAtPlayer.X + aX, kAtPlayer.Y, kAtPlayer.Z}};
}
} // namespace

TEST_CASE("Only a graph that tracks and has a target point is read as looking", "[npc_head_track]")
{
    const Look cLook = ReadLook(true, true, false, true, kAtPlayer);
    CHECK(cLook.Flags == kTracking);
    CHECK(cLook.At.X == kAtPlayer.X);
    CHECK(ReadLook(true, true, true, true, kAtPlayer).Flags == (kTracking | kSpine));

    // Not tracking, a graph without the flag (a creature's), or without a target point: not looking.
    CHECK(ReadLook(true, false, true, true, kAtPlayer).Flags == 0);
    CHECK(ReadLook(false, true, false, true, kAtPlayer).Flags == 0);
    CHECK(ReadLook(true, true, false, false, kAtPlayer).Flags == 0);

    // A point that is not a number is never sent.
    CHECK(ReadLook(true, true, false, true, {std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f}).Flags == 0);
    CHECK(ReadLook(true, true, false, true, {0.f, std::numeric_limits<float>::infinity(), 0.f}).Flags == 0);
}

TEST_CASE("The owner sends a look when it starts, changes, moves or at the heartbeat, and one word when it ends", "[npc_head_track]")
{
    Outbox outbox;

    // The first round is a heartbeat: only the looking NPC is sent, an NPC that never looked sends nothing.
    outbox.Put(7, kLooking, 3);
    outbox.Put(8, kNotLooking, 3);
    auto words = outbox.Take(0);
    REQUIRE(words.size() == 1);
    CHECK(words[0].NpcId == 7);
    CHECK(words[0].What.Flags == kTracking);

    // Unchanged, or moved by less than kMoveToSend: nothing before the heartbeat.
    outbox.Put(7, kLooking, 3);
    CHECK(outbox.Take(100).empty());
    outbox.Put(7, MovedBy(kMoveToSend * 0.5f), 3);
    CHECK(outbox.Take(200).empty());

    // Small moves add up: measured from the point last sent, the next one is over the threshold.
    outbox.Put(7, MovedBy(kMoveToSend * 1.25f), 3);
    words = outbox.Take(300);
    REQUIRE(words.size() == 1);
    CHECK(words[0].What.At.X == kAtPlayer.X + kMoveToSend * 1.25f);

    // The spine flag flipping is a change.
    outbox.Put(7, {kTracking | kSpine, MovedBy(kMoveToSend * 1.25f).At}, 3);
    REQUIRE(outbox.Take(400).size() == 1);

    // A new epoch is a change too.
    outbox.Put(7, {kTracking | kSpine, MovedBy(kMoveToSend * 1.25f).At}, 4);
    words = outbox.Take(500);
    REQUIRE(words.size() == 1);
    CHECK(words[0].OwnershipEpoch == 4);

    // The heartbeat says an unchanged look again.
    outbox.Put(7, {kTracking | kSpine, MovedBy(kMoveToSend * 1.25f).At}, 4);
    CHECK(outbox.Take(kHeartbeatMs - 1).empty());
    outbox.Put(7, {kTracking | kSpine, MovedBy(kMoveToSend * 1.25f).At}, 4);
    REQUIRE(outbox.Take(kHeartbeatMs).size() == 1);

    // It stops looking: one "not looking", then nothing, even at a heartbeat.
    outbox.Put(7, kNotLooking, 4);
    words = outbox.Take(kHeartbeatMs + 100);
    REQUIRE(words.size() == 1);
    CHECK(words[0].What.Flags == 0);
    outbox.Put(7, kNotLooking, 4);
    CHECK(outbox.Take(3 * kHeartbeatMs).empty());
}

TEST_CASE("An NPC left out of a round is forgotten without a word", "[npc_head_track]")
{
    Outbox outbox;
    outbox.Put(7, kLooking, 3);
    REQUIRE(outbox.Take(0).size() == 1);

    // Gone, or no longer this game's: its new owner or the copies' expiry ends the look.
    CHECK(outbox.Take(100).empty());
    CHECK(outbox.Take(kHeartbeatMs).empty());

    // Back later, it is a new look.
    outbox.Put(7, kLooking, 5);
    REQUIRE(outbox.Take(kHeartbeatMs + 100).size() == 1);
}

TEST_CASE("The server takes a look only from the NPC's owner at its epoch", "[npc_head_track]")
{
    const Word cWord = WordOf(7, kLooking, 3);
    CHECK(ServerTakes(true, false, true, 3, cWord, true, kNever) == ServerVerdict::kRelay);

    // A player's character, or no character: skipped.
    CHECK(ServerTakes(true, true, true, 3, cWord, true, kNever) == ServerVerdict::kSkip);
    CHECK(ServerTakes(false, false, true, 3, cWord, true, kNever) == ServerVerdict::kSkip);

    // Not the owner, another epoch, or epoch 0: an epoch drop.
    CHECK(ServerTakes(true, false, false, 3, cWord, true, kNever) == ServerVerdict::kEpochDrop);
    CHECK(ServerTakes(true, false, true, 4, cWord, true, kNever) == ServerVerdict::kEpochDrop);
    CHECK(ServerTakes(true, false, true, 0, WordOf(7, kLooking, 0), true, kNever) == ServerVerdict::kEpochDrop);

    // A point that is not a number is dropped.
    CHECK(ServerTakes(true, false, true, 3, WordOf(7, {kTracking, {std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f}}), true, kNever) == ServerVerdict::kSkip);

    // An unchanged look goes on at most every kRelayEveryMs; a changed one and a stop at once.
    CHECK(ServerTakes(true, false, true, 3, cWord, false, kRelayEveryMs - 1) == ServerVerdict::kStore);
    CHECK(ServerTakes(true, false, true, 3, cWord, false, kRelayEveryMs) == ServerVerdict::kRelay);
    CHECK(ServerTakes(true, false, true, 3, WordOf(7, kNotLooking), true, 0) == ServerVerdict::kRelay);
}

TEST_CASE("A copy takes a look only at its own epoch, and lets it go after kExpireMs", "[npc_head_track]")
{
    CHECK(CopyTakes(true, true, 3, 3) == CopyVerdict::kTake);
    CHECK(CopyTakes(false, false, 0, 3) == CopyVerdict::kUnknown);
    CHECK(CopyTakes(true, false, 0, 3) == CopyVerdict::kOwn);
    CHECK(CopyTakes(true, true, 4, 3) == CopyVerdict::kEpochMiss);
    CHECK(CopyTakes(true, true, 0, 0) == CopyVerdict::kEpochMiss);

    CHECK_FALSE(CopyExpired(1000, 1000 + kExpireMs));
    CHECK(CopyExpired(1000, 1000 + kExpireMs + 1));
}

TEST_CASE("Only a copy's graph is written, and only while its owner's look lasts", "[npc_head_track]")
{
    // A copy that is ready and not held by a fight looks.
    CHECK(CopyAction(true, false, kTracking, false, true, false, false) == CopyStep::kLook);
    CHECK(CopyAction(true, false, kTracking, false, true, false, true) == CopyStep::kLook);

    // Not loaded yet, or the combat look holds it: nothing written, the word is kept.
    CHECK(CopyAction(true, false, kTracking, false, false, false, false) == CopyStep::kWait);
    CHECK(CopyAction(true, false, kTracking, false, true, true, true) == CopyStep::kWait);

    // The owner stopped, or no word for kExpireMs: a copy we wrote is told to stop; one we never wrote is just forgotten.
    CHECK(CopyAction(true, false, 0, false, true, false, true) == CopyStep::kStop);
    CHECK(CopyAction(true, false, kTracking, true, true, false, true) == CopyStep::kStop);
    CHECK(CopyAction(true, false, 0, false, true, false, false) == CopyStep::kForget);
    CHECK(CopyAction(true, false, kTracking, true, false, false, true) == CopyStep::kForget);

    // This game gained the NPC: never a new look, whatever the word says. A look this game wrote is stopped once, so that no
    // value of ours is left in an NPC this game now runs (and saves), or read back from it and sent on as its own.
    CHECK(CopyAction(false, false, kTracking, false, true, false, true) == CopyStep::kStop);
    CHECK(CopyAction(false, false, kTracking, false, true, false, false) == CopyStep::kForget);
    CHECK(CopyAction(false, false, 0, true, true, false, false) == CopyStep::kForget);
    CHECK(CopyAction(false, false, kTracking, false, false, false, true) == CopyStep::kForget);

    // Dead: nothing is written.
    for (const bool cWritten : {false, true})
        CHECK(CopyAction(true, true, kTracking, false, true, false, cWritten) == CopyStep::kForget);
}

TEST_CASE("Combat keeps the look flags while it points the copy's head: an ambient stop or expiry writes nothing", "[npc_head_track]")
{
    // Combat holds the copy: a look that is still on waits, and one that is over (stopped, expired, stale, or the NPC gained)
    // writes no stop that would clear combat's flags. A copy we wrote a look into is told to stop once combat lets go.
    for (const bool cReady : {false, true})
    {
        CHECK(CopyAction(true, false, kTracking, false, cReady, true, true) == CopyStep::kWait);
        CHECK(CopyAction(true, false, 0, false, cReady, true, true) == CopyStep::kWait);
        CHECK(CopyAction(true, false, kTracking, true, cReady, true, true) == CopyStep::kWait);
        CHECK(CopyAction(true, false, kTracking, false, cReady, true, true, true) == CopyStep::kWait);
        CHECK(CopyAction(false, false, kTracking, false, cReady, true, true) == CopyStep::kWait);

        // Nothing of ours to undo: the word just goes.
        CHECK(CopyAction(true, false, 0, false, cReady, true, false) == CopyStep::kForget);
        CHECK(CopyAction(true, false, kTracking, true, cReady, true, false) == CopyStep::kForget);
        CHECK(CopyAction(true, false, kTracking, false, cReady, true, false, true) == CopyStep::kForget);
    }

    // The same enemy all along: the stop that came in the fight is still not written while combat looks, and is written once after.
    CHECK(CopyAction(true, false, 0, false, true, true, true) == CopyStep::kWait);
    CHECK(CopyAction(true, false, 0, false, true, true, true) == CopyStep::kWait);
    CHECK(CopyAction(true, false, 0, false, true, false, true) == CopyStep::kStop);

    // A look still on when combat ends is shown again from the owner's point; a dead copy is never written.
    CHECK(CopyAction(true, false, kTracking, false, true, true, true) == CopyStep::kWait);
    CHECK(CopyAction(true, false, kTracking, false, true, false, true) == CopyStep::kLook);
    CHECK(CopyAction(true, true, kTracking, false, true, true, true) == CopyStep::kForget);
}

TEST_CASE("A look whose owner or copy changed is stopped once and not written again", "[npc_head_track]")
{
    // The word's epoch is not the copy's: a written look is told to stop; one never written, or not ready, just goes.
    CHECK(CopyAction(true, false, kTracking, false, true, false, true, true) == CopyStep::kStop);
    CHECK(CopyAction(true, false, kTracking, false, true, false, false, true) == CopyStep::kForget);
    CHECK(CopyAction(true, false, kTracking, false, false, false, true, true) == CopyStep::kForget);

    // Never a look from a stale word, whatever it says.
    CHECK(CopyAction(true, false, kTracking, false, true, false, false, true) != CopyStep::kLook);
    CHECK(CopyAction(true, false, kTracking, false, true, false, true, false) == CopyStep::kLook);
}

TEST_CASE("The owner's NPCs are read a few each frame, and a round starts every kReadEveryMs at the soonest", "[npc_head_track]")
{
    ReadRound round;
    CHECK(round.Due(0));
    CHECK_FALSE(round.Active());

    // 60 NPCs: read in slices of kPerFrame, none twice, none missed; the words go when the last one is read.
    round.Start(1000, 60);
    CHECK(round.Active());
    CHECK_FALSE(round.Due(5000));

    size_t next = 0;
    size_t frames = 0;
    while (!round.Finished())
    {
        const ReadRound::Slice cSlice = round.Next();
        CHECK(cSlice.Begin == next);
        CHECK(cSlice.End - cSlice.Begin <= ReadRound::kPerFrame);
        CHECK(cSlice.End > cSlice.Begin);
        next = cSlice.End;
        ++frames;
    }
    CHECK(next == 60);
    CHECK(frames == (60 + ReadRound::kPerFrame - 1) / ReadRound::kPerFrame);

    // Ended: the next one starts kReadEveryMs after this one started, not after it finished.
    round.End();
    CHECK_FALSE(round.Active());
    CHECK_FALSE(round.Due(1000 + kReadEveryMs - 1));
    CHECK(round.Due(1000 + kReadEveryMs));

    // No NPCs at all: still a round, so that the looks that ended get their word.
    round.Start(2000, 0);
    CHECK(round.Finished());
    const ReadRound::Slice cNone = round.Next();
    CHECK(cNone.Begin == cNone.End);
}

TEST_CASE("A round left unfinished sends nothing of its own the next time", "[npc_head_track]")
{
    Outbox outbox;
    outbox.Put(7, kLooking, 3);
    REQUIRE(outbox.Take(0).size() == 1);

    // Half a round was read, then the connection ended: its looks are dropped, not carried into the next round.
    outbox.Put(7, MovedBy(500.f), 3);
    outbox.Abandon();
    outbox.Put(7, kLooking, 3);
    CHECK(outbox.Take(100).empty());
}

TEST_CASE("An unchanged shown look is written again only at intervals", "[npc_head_track]")
{
    const Point cHere{10.f, 20.f, 30.f};
    const Point cNearby{10.f + kWriteEpsilon * 0.5f, 20.f, 30.f};
    const Point cMoved{10.f + kWriteEpsilon * 2.f, 20.f, 30.f};

    // Nothing shown yet, other flags, or a move over kWriteEpsilon: written now.
    CHECK(NeedsWrite(false, kTracking, kTracking, cHere, cHere, 16.f));
    CHECK(NeedsWrite(true, kTracking, kTracking | kSpine, cHere, cHere, 16.f));
    CHECK(NeedsWrite(true, kTracking, kTracking, cHere, cMoved, 16.f));

    // Hardly moved: not written, until kRewriteMs has passed (a reloaded graph gets the look back).
    CHECK_FALSE(NeedsWrite(true, kTracking, kTracking, cHere, cNearby, 16.f));
    CHECK_FALSE(NeedsWrite(true, kTracking, kTracking, cHere, cHere, kRewriteMs - 1.f));
    CHECK(NeedsWrite(true, kTracking, kTracking, cHere, cHere, kRewriteMs));

    // Eased over the time since the last write, small steps add up to the same point as steps of every frame.
    const Point cFrom{0.f, 0.f, 0.f};
    const Point cTo{100.f, 0.f, 0.f};
    Point stepped = cFrom;
    for (int i = 0; i < 16; ++i)
        stepped = Ease(true, stepped, cTo, 16.f);
    CHECK(Ease(true, cFrom, cTo, 16.f * 16.f).X == Approx(stepped.X).epsilon(0.001));
}

TEST_CASE("The copy's head eases towards its owner's point and snaps to a new target", "[npc_head_track]")
{
    const Point cFrom{0.f, 0.f, 0.f};
    const Point cTo{100.f, 0.f, 0.f};

    // The first look is taken at once.
    CHECK(Ease(false, cFrom, cTo, 16.f).X == 100.f);

    // One time constant gets about two thirds of the way; frames add up to the same as one long step.
    const Point cOne = Ease(true, cFrom, cTo, kEaseMs);
    CHECK(cOne.X == Approx(100.f * (1.f - std::exp(-1.f))).epsilon(0.001));
    Point stepped = cFrom;
    for (int i = 0; i < 10; ++i)
        stepped = Ease(true, stepped, cTo, kEaseMs / 10.f);
    CHECK(stepped.X == Approx(cOne.X).epsilon(0.001));

    // Never past the target, and there after half a second.
    CHECK(Ease(true, cFrom, cTo, 10000.f).X <= 100.f);
    CHECK(Ease(true, cFrom, cTo, 500.f).X > 98.f);

    // A jump over kSnapDistance is a new target: taken at once.
    const Point cFar{kSnapDistance + 1.f, 0.f, 0.f};
    CHECK(Ease(true, cFrom, cFar, 16.f).X == cFar.X);

    // No time passed: the head stays where it was.
    CHECK(Ease(true, cFrom, cTo, 0.f).X == 0.f);
}
