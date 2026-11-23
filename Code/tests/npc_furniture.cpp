#include <catch2/catch.hpp>

#include <NpcFurniture.h>

#include <limits>
#include <tuple>

using namespace NpcFurniture;

namespace
{
constexpr uint64_t kNever = std::numeric_limits<uint64_t>::max();

constexpr Seat kChair{2, 0x1A2B3, 1, kStateSitting};
constexpr Seat kOtherChair{2, 0x1A2B4, 0, kStateSitting};
constexpr Seat kBed{2, 0x2C3D4, 0, kStateSleeping};

Word WordOf(const uint32_t aNpcId, const Seat& acSeat, const uint32_t aEpoch = 3) noexcept
{
    return {aNpcId, acSeat, aEpoch};
}
} // namespace

TEST_CASE("Only a settled seat in furniture the games share is read as seated", "[npc_furniture]")
{
    CHECK(ReadSeat(kStateSitting, true, true, false, true, 2, 0x1A2B3, 1) == kChair);
    CHECK(ReadSeat(kStateSleeping, true, true, false, true, 2, 0x2C3D4, 0) == kBed);

    // On the way in or out (wanting to sit, waiting for the animation, wanting to stand), on a horse, or in nothing: not seated.
    for (const uint8_t cState : {uint8_t{0}, uint8_t{1}, uint8_t{2}, uint8_t{4}, uint8_t{5}, uint8_t{6}, uint8_t{8}})
        CHECK(ReadSeat(cState, true, true, false, true, 2, 0x1A2B3, 1).State == kStateNone);
    CHECK(ReadSeat(kStateSitting, false, false, false, false, 0, 0, 0).State == kStateNone);

    // Furniture placed at run time has no name the other games share.
    CHECK(ReadSeat(kStateSitting, true, true, false, false, 0, 0, 1).State == kStateNone);

    // A rider reads as seated, and its process can name its horse: a reference that is not furniture, or a mounted NPC, is no seat.
    CHECK(ReadSeat(kStateSitting, true, false, false, true, 2, 0x1A2B3, 0).State == kStateNone);
    CHECK(ReadSeat(kStateSitting, true, true, true, true, 2, 0x1A2B3, 1).State == kStateNone);
}

TEST_CASE("Only sitting and sleeping are settled, whatever the furniture", "[npc_furniture]")
{
    for (uint8_t state = 0; state < 16; ++state)
        CHECK(IsSettled(state) == (state == kStateSitting || state == kStateSleeping));
}

TEST_CASE("The owner sends a seat when it changes, every seat at a heartbeat, and one word when an NPC gets up", "[npc_furniture]")
{
    Outbox outbox;
    uint64_t now = 1000;

    // The first round is a heartbeat: every seated NPC goes, and one that is not seated says nothing.
    outbox.Put(7, kChair, 3);
    outbox.Put(8, kBed, 3);
    outbox.Put(9, {}, 3);
    CHECK(outbox.Take(now).size() == 2);

    // Nothing changed: nothing goes.
    now += kReadEveryMs;
    outbox.Put(7, kChair, 3);
    outbox.Put(8, kBed, 3);
    outbox.Put(9, {}, 3);
    CHECK(outbox.Take(now).empty());

    // One moved to another chair: only it goes.
    now += kReadEveryMs;
    outbox.Put(7, kOtherChair, 3);
    outbox.Put(8, kBed, 3);
    auto words = outbox.Take(now);
    REQUIRE(words.size() == 1);
    CHECK(words[0].NpcId == 7);
    CHECK(words[0].Where == kOtherChair);

    // The same chair at a new epoch is a change too.
    now += kReadEveryMs;
    outbox.Put(7, kOtherChair, 4);
    outbox.Put(8, kBed, 3);
    words = outbox.Take(now);
    REQUIRE(words.size() == 1);
    CHECK(words[0].OwnershipEpoch == 4);

    // One got up: one "not seated", then nothing for it.
    now += kReadEveryMs;
    outbox.Put(7, kOtherChair, 4);
    outbox.Put(8, {}, 3);
    words = outbox.Take(now);
    REQUIRE(words.size() == 1);
    CHECK(words[0].NpcId == 8);
    CHECK(words[0].Where.State == kStateNone);
    now += kReadEveryMs;
    outbox.Put(7, kOtherChair, 4);
    outbox.Put(8, {}, 3);
    CHECK(outbox.Take(now).empty());

    // At the heartbeat the seated one goes again, for a game that loaded its copy later.
    now = 1000 + kHeartbeatMs;
    outbox.Put(7, kOtherChair, 4);
    outbox.Put(8, {}, 3);
    words = outbox.Take(now);
    REQUIRE(words.size() == 1);
    CHECK(words[0].NpcId == 7);

    // An NPC left out of a round (gone, or handed on) is forgotten without a word; seen again seated, it is new.
    now += kReadEveryMs;
    CHECK(outbox.Take(now).empty());
    now += kReadEveryMs;
    outbox.Put(7, kOtherChair, 4);
    CHECK(outbox.Take(now).size() == 1);
}

TEST_CASE("The server takes a seat only from the NPC's owner at its epoch, and relays a changed one at once", "[npc_furniture]")
{
    const Word cWord = WordOf(7, kChair, 3);

    CHECK(ServerTakes(false, false, true, 3, cWord, true, kNever) == ServerVerdict::kSkip);
    CHECK(ServerTakes(true, true, true, 3, cWord, true, kNever) == ServerVerdict::kSkip);
    CHECK(ServerTakes(true, false, false, 3, cWord, true, kNever) == ServerVerdict::kEpochDrop);
    CHECK(ServerTakes(true, false, true, 4, cWord, true, kNever) == ServerVerdict::kEpochDrop);
    CHECK(ServerTakes(true, false, true, 0, WordOf(7, kChair, 0), true, kNever) == ServerVerdict::kEpochDrop);

    // A state no game sends, or a seat with no furniture: dropped.
    CHECK(ServerTakes(true, false, true, 3, WordOf(7, {2, 0x1A2B3, 1, 2}), true, kNever) == ServerVerdict::kSkip);
    CHECK(ServerTakes(true, false, true, 3, WordOf(7, {2, 0, 1, kStateSitting}), true, kNever) == ServerVerdict::kSkip);

    // Changed: relayed at once. Unchanged: stored until kRelayEveryMs went, which lets the heartbeat through.
    CHECK(ServerTakes(true, false, true, 3, cWord, true, 10) == ServerVerdict::kRelay);
    CHECK(ServerTakes(true, false, true, 3, cWord, false, 10) == ServerVerdict::kStore);
    CHECK(ServerTakes(true, false, true, 3, cWord, false, kRelayEveryMs) == ServerVerdict::kRelay);

    // "Not seated" is a word too.
    CHECK(ServerTakes(true, false, true, 3, WordOf(7, {}), true, 10) == ServerVerdict::kRelay);
}

TEST_CASE("A copy takes a seat only as a copy at the word's epoch, and lets it go after 5 s of silence", "[npc_furniture]")
{
    CHECK(CopyTakes(false, false, 0, 3) == CopyVerdict::kUnknown);
    CHECK(CopyTakes(true, false, 0, 3) == CopyVerdict::kOwn);
    CHECK(CopyTakes(true, true, 3, 4) == CopyVerdict::kEpochMiss);
    CHECK(CopyTakes(true, true, 3, 0) == CopyVerdict::kEpochMiss);
    CHECK(CopyTakes(true, true, 3, 3) == CopyVerdict::kTake);

    CHECK_FALSE(CopyExpired(1000, 1000 + kExpireMs));
    CHECK(CopyExpired(1000, 1001 + kExpireMs));
    // Two heartbeats can be lost before a seated copy stands.
    CHECK(2 * kHeartbeatMs < kExpireMs);
}

TEST_CASE("A copy is seated, kept, stood and let go as its owner's NPC is", "[npc_furniture]")
{
    // Owner seated, copy standing and loaded: seat it. Not loaded (or the furniture is not): wait for the next round.
    CHECK(CopyAction(kStateSitting, false, kStateNone, false, false, true, true) == CopyStep::kSeat);
    CHECK(CopyAction(kStateSleeping, false, kStateNone, false, false, true, true) == CopyStep::kSeat);
    CHECK(CopyAction(kStateSitting, false, kStateNone, false, false, false, true) == CopyStep::kWait);

    // Already where the owner says: nothing to do, whoever seated it.
    CHECK(CopyAction(kStateSitting, false, kStateSitting, true, true, true, true) == CopyStep::kKeep);
    CHECK(CopyAction(kStateSitting, false, kStateSitting, true, false, true, true) == CopyStep::kKeep);

    // The owner moved to other furniture, or from a chair to a bed: stand the copy first if this game seated it, then seat it.
    CHECK(CopyAction(kStateSitting, false, kStateSitting, false, true, true, true) == CopyStep::kStand);
    CHECK(CopyAction(kStateSleeping, false, kStateSitting, true, true, true, true) == CopyStep::kStand);

    // Something else holds the copy (a horse, a cart, a scene): left alone until it lets go.
    CHECK(CopyAction(kStateSitting, false, kStateSitting, false, false, true, true) == CopyStep::kWait);
    CHECK(CopyAction(kStateSitting, false, 2, false, false, true, true) == CopyStep::kWait);

    // The owner's NPC got up, or the owner went quiet: stand a copy this game seated, and forget the rest.
    CHECK(CopyAction(kStateNone, false, kStateSitting, true, true, true, true) == CopyStep::kStand);
    CHECK(CopyAction(kStateSitting, true, kStateSitting, true, true, true, true) == CopyStep::kStand);
    CHECK(CopyAction(kStateNone, false, kStateSitting, false, false, true, true) == CopyStep::kForget);
    CHECK(CopyAction(kStateNone, false, kStateNone, false, false, true, true) == CopyStep::kForget);
    CHECK(CopyAction(kStateSitting, true, kStateNone, false, false, true, true) == CopyStep::kForget);
}

TEST_CASE("A copy whose owner's NPC got up is given 2 s to get up by its replayed get-up", "[npc_furniture]")
{
    // The owner's NPC got up and the copy still sits: wait for its own get-up while the grace runs, then stand it.
    CHECK(CopyAction(kStateNone, false, kStateSitting, true, true, true, false) == CopyStep::kWait);
    CHECK(CopyAction(kStateNone, false, kStateSitting, true, true, true, true) == CopyStep::kStand);
    // Halfway up by its own animation (wanting to stand): still waiting; up by it: nothing left to do.
    CHECK(CopyAction(kStateNone, false, 4, false, true, true, false) == CopyStep::kWait);
    CHECK(CopyAction(kStateNone, false, kStateNone, false, false, true, false) == CopyStep::kForget);
    // The owner went quiet: no get-up is coming, so it is stood at once.
    CHECK(CopyAction(kStateSitting, true, kStateSitting, true, true, true, false) == CopyStep::kStand);
    // A move to another seat stands it at once, grace or not.
    CHECK(CopyAction(kStateSitting, false, kStateSitting, false, true, true, false) == CopyStep::kStand);

    CHECK_FALSE(StandGraceOver(0, 100000));
    CHECK_FALSE(StandGraceOver(1000, 1000 + kStandGraceMs - 1));
    CHECK(StandGraceOver(1000, 1000 + kStandGraceMs));
}

TEST_CASE("A seat the game refused is tried again at the heartbeat, not every round", "[npc_furniture]")
{
    CHECK(MaySeatAgain(0, 1000));
    CHECK_FALSE(MaySeatAgain(1000, 1000 + kReadEveryMs));
    CHECK(MaySeatAgain(1000, 1000 + kHeartbeatMs));
}

TEST_CASE("A seat is the furniture and the marker in it", "[npc_furniture]")
{
    // Same bench, the marker the word names: the same seat.
    CHECK(SameSeat(true, 1, 1, false, 0));
    // Same bench, another marker: not the same seat, so a copy this game seated is stood and seated at the word's marker.
    CHECK_FALSE(SameSeat(true, 0, 1, false, 0));
    CHECK(CopyAction(kStateSitting, false, kStateSitting, SameSeat(true, 0, 1, false, 0), true, true, true) == CopyStep::kStand);
    // Other furniture is never the same seat.
    CHECK_FALSE(SameSeat(false, 1, 1, true, 1));

    // Asked for marker 1, which another actor holds here, and given marker 2 in the same bench: kept, not stood and seated each round.
    CHECK(SameSeat(true, 2, 1, true, 2));
    CHECK(CopyAction(kStateSitting, false, kStateSitting, SameSeat(true, 2, 1, true, 2), true, true, true) == CopyStep::kKeep);
    // ...but not once the word names marker 0 instead.
    CHECK_FALSE(SameSeat(true, 2, 0, false, 2));
}

TEST_CASE("What the game reads back from a seated copy", "[npc_furniture]")
{
    CHECK(ReadBack(kStateSitting, kStateSitting, true, 1, 1) == SeatReadBack::kAsAsked);
    // Seated, but at another marker, or in other furniture.
    CHECK(ReadBack(kStateSitting, kStateSitting, true, 1, 2) == SeatReadBack::kElsewhere);
    CHECK(ReadBack(kStateSitting, kStateSitting, false, 1, 1) == SeatReadBack::kElsewhere);
    // The marker taken by another actor and the game would not seat it: still standing.
    CHECK(ReadBack(kStateSitting, kStateNone, true, 1, 0) == SeatReadBack::kRefused);
    CHECK(ReadBack(kStateSleeping, kStateSitting, true, 0, 0) == SeatReadBack::kRefused);
}

TEST_CASE("A word kept from an NPC's earlier owner seats no copy, and can still stand one", "[npc_furniture]")
{
    CHECK(CopyActionUnder(true, CopyStep::kSeat) == CopyStep::kSeat);
    CHECK(CopyActionUnder(false, CopyStep::kSeat) == CopyStep::kWait);
    CHECK(CopyActionUnder(false, CopyStep::kStand) == CopyStep::kStand);
    CHECK(CopyActionUnder(false, CopyStep::kKeep) == CopyStep::kKeep);
    CHECK(CopyActionUnder(false, CopyStep::kForget) == CopyStep::kForget);
}

TEST_CASE("A copy this game seated is written to the save standing, and seated again after", "[npc_furniture]")
{
    // Sitting (3) and sleeping (7) in bits 14-17, beside other flags the save keeps as they are.
    constexpr uint32_t cOther = 0x00200041;
    constexpr uint32_t cSitting = cOther | (uint32_t{kStateSitting} << 14);
    constexpr uint32_t cSleeping = cOther | (uint32_t{kStateSleeping} << 14);

    CHECK(HideSeatInSave(true, true, cSitting));
    CHECK(HideSeatInSave(true, true, cSleeping));
    CHECK(WithoutSeat(cSitting) == cOther);
    CHECK(WithoutSeat(cSleeping) == cOther);

    // Only a copy this game seated: not this game's own NPC (taken over since, or never another game's), not a copy something
    // else seats, not one standing already.
    CHECK_FALSE(HideSeatInSave(true, false, cSitting));
    CHECK_FALSE(HideSeatInSave(false, true, cSitting));
    CHECK_FALSE(HideSeatInSave(true, true, cOther));

    // Put back after the write, with any other flag the write changed kept: one set that was clear, one cleared that was set.
    constexpr uint32_t cSetSince = 0x00000100;
    constexpr uint32_t cClearedSince = 0x00000040;
    static_assert((cOther & cSetSince) == 0 && (cOther & cClearedSince) != 0);
    CHECK(WithSeat(WithoutSeat(cSitting), cSitting) == cSitting);
    CHECK(WithSeat((WithoutSeat(cSitting) | cSetSince) & ~cClearedSince, cSitting) == ((cSitting | cSetSince) & ~cClearedSince));
    CHECK(WithSeat(cOther, cSleeping) == cSleeping);
}

TEST_CASE("A seated copy's save is written standing in no furniture, and all of it is put back however the save is left", "[npc_furniture]")
{
    constexpr uint32_t cOther = 0x00200041;
    constexpr uint32_t cSitting = cOther | (uint32_t{kStateSitting} << 14);
    uint32_t flags1 = cSitting;
    uint32_t furniture = 0x00123456;
    uint32_t marker = 2;

    {
        SeatSaveMask mask;
        mask.Lift(&flags1, &furniture, &marker, true, true);
        REQUIRE(mask.Lifted());
        // What the save writes.
        CHECK(flags1 == cOther);
        CHECK(furniture == kNoFurnitureHandle);
        CHECK(marker == kNoFurnitureMarker);
        // The serializer changes an unrelated flag meanwhile.
        flags1 |= 0x100;
    }
    CHECK(flags1 == (cSitting | 0x100));
    CHECK(furniture == 0x00123456);
    CHECK(marker == 2);

    // A serializer that throws: everything is put back on the way out.
    flags1 = cSitting;
    const auto cThrowingSave = [&]()
    {
        SeatSaveMask mask;
        mask.Lift(&flags1, &furniture, &marker, true, true);
        throw 1;
    };
    CHECK_THROWS(cThrowingSave());
    CHECK(flags1 == cSitting);
    CHECK(furniture == 0x00123456);
    CHECK(marker == 2);

    // Not this game's seat, not a copy, or standing: nothing is touched.
    for (const auto& [cSeatedHere, cIsCopy, cFlags] : {std::tuple{false, true, cSitting}, std::tuple{true, false, cSitting}, std::tuple{true, true, cOther}})
    {
        flags1 = cFlags;
        SeatSaveMask mask;
        mask.Lift(&flags1, &furniture, &marker, cSeatedHere, cIsCopy);
        CHECK_FALSE(mask.Lifted());
        CHECK(flags1 == cFlags);
        CHECK(furniture == 0x00123456);
        CHECK(marker == 2);
    }

    // No process holding furniture: only the seat is lifted.
    flags1 = cSitting;
    {
        SeatSaveMask mask;
        mask.Lift(&flags1, nullptr, nullptr, true, true);
        CHECK(mask.Lifted());
        CHECK(flags1 == cOther);
    }
    CHECK(flags1 == cSitting);
}

TEST_CASE("Only the receipt of the work a copy waits for is taken", "[npc_furniture]")
{
    // Work 4 under ticket 7 on copy handle 0x100, out since 1000.
    constexpr WorkId cOut{4, 7, 0x100};
    CHECK(ReceiptIsFor(true, 1000, cOut, cOut));
    // An earlier piece of work, given up on and replaced by work 4.
    CHECK_FALSE(ReceiptIsFor(true, 1000, cOut, {3, 7, 0x100}));
    // The same serial under another decision, or for another actor on the copy's server id.
    CHECK_FALSE(ReceiptIsFor(true, 1000, cOut, {4, 6, 0x100}));
    CHECK_FALSE(ReceiptIsFor(true, 1000, cOut, {4, 7, 0x200}));
    // Nothing out any more (given up on after kWorkLostMs), or the word dropped meanwhile.
    CHECK_FALSE(ReceiptIsFor(true, 0, cOut, cOut));
    CHECK_FALSE(ReceiptIsFor(false, 1000, cOut, cOut));
    CHECK_FALSE(ReceiptIsFor(true, 1000, {}, {}));
}

TEST_CASE("A receipt for a copy's earlier seat state is not taken by the one made again", "[npc_furniture]")
{
    WorkSerials serials;

    // The first seat state of server character 0x50: its first seat goes out under ticket 1 on copy handle 0x100.
    const WorkId cOldWork{serials.Next(), 1, 0x100};

    // Its word dropped, the copy's seat state is made again for the same server character, and its first work goes out: a
    // new decision (ticket 2), on the same copy.
    const WorkId cNewWork{serials.Next(), 2, 0x100};
    CHECK(cNewWork.Serial != cOldWork.Serial);
    // The old state's receipt arrives now: not this work's.
    CHECK_FALSE(ReceiptIsFor(true, 2000, cNewWork, cOldWork));
    CHECK(ReceiptIsFor(true, 2000, cNewWork, cNewWork));

    // Even with the serials equal (a count per copy restarting at 1), the decision and the copy tell them apart.
    CHECK_FALSE(ReceiptIsFor(true, 2000, {1, 2, 0x100}, {1, 1, 0x100}));
    // The copy made again too (another actor, same server id): its stand undoes nothing of the old actor's.
    CHECK_FALSE(ReceiptIsFor(true, 2000, {1, 0, 0x200}, {1, 0, 0x100}));
}

TEST_CASE("A copy left away from its owner's place does not take its owner's way into a seat", "[npc_furniture]")
{
    using NpcFurniture::IsSeatReplay;
    using NpcFurniture::SkipSeatReplay;

    // Ways into a seat, by name or by the owner's seated state word.
    CHECK(IsSeatReplay("IdleChairEnterInstant", 0));
    CHECK(IsSeatReplay("idlebedenter", 0));
    CHECK(IsSeatReplay("IdleLeanTableEnter", 0));
    CHECK(IsSeatReplay("moveStop", NpcFurniture::kStateSitting << 14));
    // Anything else.
    CHECK_FALSE(IsSeatReplay("moveStart", 0));
    CHECK_FALSE(IsSeatReplay("IdleChairExit", 0));
    CHECK_FALSE(IsSeatReplay(nullptr, 0));

    // Ralof, rc7.1: left on the road 14000 units from the bench his owner's game has him on.
    CHECK(SkipSeatReplay(true, true, true, 14000.f));
    // Moved to his owner's place since, or near it: played.
    CHECK_FALSE(SkipSeatReplay(true, false, true, 14000.f));
    CHECK_FALSE(SkipSeatReplay(true, true, true, 200.f));
    // Not a seat.
    CHECK_FALSE(SkipSeatReplay(false, true, true, 14000.f));
}

TEST_CASE("A copy in another cell or world space than its owner's place is away whatever its coordinates", "[npc_furniture]")
{
    using NpcFurniture::SameSpace;
    using NpcFurniture::SkipSeatReplay;

    constexpr uint32_t cTamriel = 0x3C;
    constexpr uint32_t cSovngarde = 0x2EE41;
    constexpr uint32_t cBanneredMare = 0x1605E;
    constexpr uint32_t cSleepingGiant = 0x133C6;

    // Two interiors whose local coordinates are close: the owner sits in the Sleeping Giant, the copy was left in the Bannered Mare
    // 150 units from those coordinates. Away: its owner's way into a seat is not played there.
    CHECK_FALSE(SameSpace(0, cSleepingGiant, 0, cBanneredMare));
    CHECK(SkipSeatReplay(true, true, SameSpace(0, cSleepingGiant, 0, cBanneredMare), 150.f));
    // The same interior, near the owner's seat: played; far from it: not.
    CHECK(SameSpace(0, cBanneredMare, 0, cBanneredMare));
    CHECK_FALSE(SkipSeatReplay(true, true, SameSpace(0, cBanneredMare, 0, cBanneredMare), 150.f));
    CHECK(SkipSeatReplay(true, true, SameSpace(0, cBanneredMare, 0, cBanneredMare), 1500.f));

    // Outside, the cells of one world space share its coordinates: distance decides.
    CHECK(SameSpace(cTamriel, 0, cTamriel, 0));
    CHECK_FALSE(SkipSeatReplay(true, true, SameSpace(cTamriel, 0, cTamriel, 0), 150.f));
    // Another world space, an interior against an exterior either way round: away.
    CHECK_FALSE(SameSpace(cSovngarde, 0, cTamriel, 0));
    CHECK_FALSE(SameSpace(cTamriel, 0, 0, cBanneredMare));
    CHECK_FALSE(SameSpace(0, cBanneredMare, cTamriel, 0));
    CHECK(SkipSeatReplay(true, true, SameSpace(0, cBanneredMare, cTamriel, 0), 150.f));
    // An owner's place this game does not have (no id here) is no space the copy is in.
    CHECK_FALSE(SameSpace(0, 0, 0, 0));
    CHECK_FALSE(SameSpace(0, 0, cTamriel, 0));
}

TEST_CASE("The action replay's write over a copy's sit/sleep bits does not make it seated again or leave it in its seat", "[npc_furniture]")
{
    // Seated by this game in a chair, its bits written standing by a replayed action: the seat it holds still counts, so it is kept
    // while its owner sits and not put in the chair a second time.
    const uint8_t wiped = CopySeatState(kStateNone, true, true, kStateSitting);
    CHECK(wiped == kStateSitting);
    CHECK(CopyAction(kStateSitting, false, wiped, true, true, true, true) == CopyStep::kKeep);
    // Its owner's NPC got up and the bits are still wiped: the copy is given its grace, then stood, rather than forgotten in the chair.
    CHECK(CopyAction(kStateNone, false, wiped, true, true, true, false) == CopyStep::kWait);
    CHECK(CopyAction(kStateNone, false, wiped, true, true, true, true) == CopyStep::kStand);
    // A word that ran out stands it at once.
    CHECK(CopyAction(kStateSitting, true, wiped, true, true, true, false) == CopyStep::kStand);

    // Written seated on a copy that holds no furniture: it is standing, and is seated as its owner says, not left waiting.
    const uint8_t stray = CopySeatState(kStateSitting, false, false, kStateNone);
    CHECK(stray == kStateNone);
    CHECK(CopyAction(kStateSitting, false, stray, false, false, true, true) == CopyStep::kSeat);
    CHECK(CopyAction(kStateNone, false, stray, false, false, true, true) == CopyStep::kForget);

    // Left as the game reads it otherwise: seated in furniture, halfway up, standing in none, or in a seat that is not ours (a scene).
    CHECK(CopySeatState(kStateSitting, true, true, kStateSitting) == kStateSitting);
    CHECK(CopySeatState(4, true, true, kStateSitting) == 4);
    CHECK(CopySeatState(kStateNone, false, false, kStateSitting) == kStateNone);
    CHECK(CopySeatState(kStateSitting, true, false, kStateNone) == kStateSitting);
    // Standing with furniture held that this game did not seat it in: not a seat of ours to put back.
    CHECK(CopySeatState(kStateNone, true, false, kStateSitting) == kStateNone);
    // A seat this game made in no settled state is not put back.
    CHECK(CopySeatState(kStateNone, true, true, kStateNone) == kStateNone);
}

TEST_CASE("The way out of a seat is told from the way in", "[npc_furniture]")
{
    CHECK(IsSeatExitReplay("IdleChairExitStart"));
    CHECK(IsSeatExitReplay("idlebedexitstart"));
    CHECK(IsSeatExitReplay("IdleLeanTableExit"));
    CHECK_FALSE(IsSeatExitReplay("IdleChairEnterInstant"));
    CHECK_FALSE(IsSeatExitReplay("moveStart"));
    CHECK_FALSE(IsSeatExitReplay("IdleExitCart"));
    CHECK_FALSE(IsSeatExitReplay(nullptr));
    // A way in is still no way out, and a way out no way in, whatever else is in the name.
    CHECK_FALSE(IsSeatReplay("IdleChairExitStart", 0));
    CHECK(IsSeatReplay("IdleChairEnterInstant", 0));
    CHECK(NamesSeat("IdleThroneEnter"));
    CHECK_FALSE(NamesSeat("IdleStop"));
}
