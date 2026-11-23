#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <SharedHomes.h>
#include <ReferenceSwitches.h>
#include <Structs/GameId.h>

#include <utility>

using namespace TiltedPhoques;

TEST_CASE("A home is a shared house with each player's own storage", "[sharedhomes]")
{
    using namespace SharedHomes;

    // A door, a lever or a bed in a home is used together.
    CHECK(IsShared(true, false, false));
    // A chest or a mannequin in a home stays each player's own.
    CHECK_FALSE(IsShared(true, true, false));
    // Unless the server shares homes whole.
    CHECK(IsShared(true, true, true));
    // Outside homes everything is shared, as before.
    CHECK(IsShared(false, true, false));
    CHECK(IsShared(false, false, false));

    // The game's own ids, the same in Skyrim.esm on SE and VR.
    static_assert(kNoResetZone == 0x0F90B1);
    static_assert(kMannequinRace == 0x10760A);
}

TEST_CASE("Any player's lever is shared just after they used it; the leader's switches always", "[sharedhomes]")
{
    using namespace SharedHomes;

    CHECK(SharesSwitch(true, kNever, kNever));
    CHECK(SharesSwitch(true, 60000, 0));
    // A member pulled a lever: what its script switched goes to the party.
    CHECK(SharesSwitch(false, 0, kNever));
    CHECK(SharesSwitch(false, kSwitchAfterUseMs, kNever));
    // A member's own quest switching the world long after any use of theirs stays theirs, as does one with no use at all.
    CHECK_FALSE(SharesSwitch(false, kSwitchAfterUseMs + 1, kNever));
    CHECK_FALSE(SharesSwitch(false, kNever, kNever));
}

TEST_CASE("A lever another player pulled, replayed here, is sent by their game only", "[sharedhomes]")
{
    using namespace SharedHomes;

    // This member pulled a lever 5 s ago; another player's pull replayed here 1 s ago: its switches are theirs to send.
    CHECK_FALSE(SharesSwitch(false, 5000, 1000));
    CHECK_FALSE(SharesSwitch(false, 5000, kReplayQuietMs));
    // Long enough after the replay, this member's own lever's slower switches go out again.
    CHECK(SharesSwitch(false, 5000, kReplayQuietMs + 1));
    // This member pulled after the replay: their own pull's switches go out.
    CHECK(SharesSwitch(false, 500, 1000));
    // The leader's game sends all it switches, its quests included.
    CHECK(SharesSwitch(true, kNever, 100));
}

TEST_CASE("The server takes a member's switches only after a use it saw, and the newest few of a large batch", "[sharedhomes]")
{
    using namespace SharedHomes;

    CHECK(SwitchesTaken(true, true, false, 5000) == 5000);
    CHECK(SwitchesTaken(true, false, true, 1) == 1);
    CHECK(SwitchesTaken(true, false, true, kMaxMemberSwitches) == kMaxMemberSwitches);
    // A large batch is not dropped whole: its first part, the newest switches, is taken (a member's game sends newest first).
    CHECK(SwitchesTaken(true, false, true, kMaxMemberSwitches + 100) == kMaxMemberSwitches);
    CHECK(SwitchesTaken(true, false, false, 1) == 0);
    CHECK(SwitchesTaken(false, true, true, 1) == 0);
    CHECK(SwitchesTaken(true, true, true, 0) == 0);
}

TEST_CASE("Two players toggling the same lever: the last switch the server got is kept, and a late joiner gets that", "[sharedhomes]")
{
    const GameId cBridge(0, 0x1A2B3);
    const GameId cGate(0, 0x1A2B4);
    ReferenceSwitches kept;

    // The leader raises the bridge, a member lowers it again a moment later, and opens a gate: in the order the server got them.
    kept.Set(cBridge, true);
    kept.Set(cBridge, false);
    kept.Set(cGate, true);
    CHECK(kept.Size() == 2);
    CHECK(kept.Find(cBridge) == false);
    CHECK(kept.Find(cGate) == true);

    // The first player raises it again: the last switch the server got wins.
    kept.Set(cBridge, true);
    CHECK(kept.Find(cBridge) == true);

    // What a player joining later is sent is exactly the last state of each, oldest change first.
    Vector<std::pair<GameId, bool>> joining;
    kept.ForEach([&](const GameId& acId, const bool aEnabled) { joining.push_back({acId, aEnabled}); });
    REQUIRE(joining.size() == 2);
    CHECK(joining[0] == std::pair{cGate, true});
    CHECK(joining[1] == std::pair{cBridge, true});
}

TEST_CASE("A rack's weapon is kept still only while its owner's script holds it, and a hand taking it hands it back to physics", "[sharedhomes]")
{
    using namespace SharedHomes;

    // Havok motion types: keyframed and fixed hold it still; dynamic, sphere, box and character do not.
    CHECK(IsHeldStillMotion(4));
    CHECK(IsHeldStillMotion(5));
    for (const int cType : {0, 1, 2, 3, 6, 7})
        CHECK_FALSE(IsHeldStillMotion(static_cast<uint8_t>(cType)));

    // Put on the rack by its script: kept still in every game, and for any player who comes later.
    CHECK(KeptStill(true, true));
    // Picked up by a hand and carried: a move, which hands it back to physics.
    CHECK_FALSE(KeptStill(false, true));
    // Dropped on the floor afterwards, dynamic again: lies there and falls as an item does.
    CHECK_FALSE(KeptStill(true, false));
}

TEST_CASE("A member's own quest step shares its switches as a lever does", "[sharedhomes]")
{
    using namespace SharedHomes;

    // Just after this game's own step, with no lever used.
    CHECK(SharesSwitch(false, kNever, kNever, 0));
    CHECK(SharesSwitch(false, kNever, kNever, kSwitchAfterUseMs));
    CHECK_FALSE(SharesSwitch(false, kNever, kNever, kSwitchAfterUseMs + 1));

    // Another game's step applied here since this game's own: its scripts' switches are that game's to send.
    CHECK_FALSE(SharesSwitch(false, kNever, kNever, 3000, 500));
    CHECK(SharesSwitch(false, kNever, kNever, 3000, kReplayQuietMs + 1));
    // This game's own step after the replay: its own again.
    CHECK(SharesSwitch(false, kNever, kNever, 200, 1000));

    // A quest step applied here does not mute a lever this player pulled, nor a replayed lever this game's own step.
    CHECK(SharesSwitch(false, 1000, kNever, kNever, 500));
    CHECK(SharesSwitch(false, kNever, 500, 3000, kNever));
    CHECK_FALSE(SharesSwitch(false, 1000, 500, kNever, kNever));

    // The newer of a lever use and an own step counts.
    CHECK(SharesSwitch(false, 60000, kNever, 100));
    CHECK(SharesSwitch(false, 100, kNever, 60000));
}

TEST_CASE("A game that catches up on the party's quests shares no switch, the leader's either", "[sharedhomes]")
{
    using namespace SharedHomes;

    // While the catch-up runs, and for a moment after it, as its stages' scripts switch a little later.
    CHECK(CatchUpHoldsSwitches(true, kNever));
    CHECK(CatchUpHoldsSwitches(false, 0));
    CHECK(CatchUpHoldsSwitches(false, kReplayQuietMs));
    CHECK_FALSE(CatchUpHoldsSwitches(false, kReplayQuietMs + 1));
    CHECK_FALSE(CatchUpHoldsSwitches(false, kNever));
}
