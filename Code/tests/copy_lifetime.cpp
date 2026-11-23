#include <catch2/catch.hpp>

#include <Copies/CopyLifetime.h>

#include <vector>

using namespace CopyLifetime;

TEST_CASE("a removed character's copy made here goes, the game's own actor never does", "[copylifetime]")
{
    // A copy this client made goes whatever else is true of it.
    REQUIRE(OnServerRemoval(true, true, false, false, false, true) == ServerRemoval::RemoveCopy);
    REQUIRE(OnServerRemoval(true, true, true, true, true, false) == ServerRemoval::RemoveCopy);
    // Nothing under that id: only the records go.
    REQUIRE(OnServerRemoval(false, false, false, true, false, false) == ServerRemoval::RemoveCopy);

    // The game's own actor is never removed by this client.
    for (const bool cDeleted : {false, true})
        for (const bool cGone : {false, true})
            for (const bool cDead : {false, true})
                for (const bool cStands : {false, true})
                    REQUIRE(OnServerRemoval(true, false, cDeleted, cGone, cDead, cStands) != ServerRemoval::RemoveCopy);
}

TEST_CASE("the game's own actor out of view stays that character; gone, it is announced again only while it stands alive", "[copylifetime]")
{
    REQUIRE(OnServerRemoval(true, false, false, false, false, true) == ServerRemoval::KeepAsHandedAway);
    REQUIRE(OnServerRemoval(true, false, false, false, false, false) == ServerRemoval::KeepAsHandedAway);
    // Being freed by the game: nothing to come back as.
    REQUIRE(OnServerRemoval(true, false, true, false, false, false) == ServerRemoval::LeaveToGame);

    REQUIRE(OnServerRemoval(true, false, false, true, false, true) == ServerRemoval::AnnounceAgain);
    REQUIRE(OnServerRemoval(true, false, false, true, true, true) == ServerRemoval::LeaveToGame);
    REQUIRE(OnServerRemoval(true, false, false, true, false, false) == ServerRemoval::LeaveToGame);
    REQUIRE(OnServerRemoval(true, false, true, true, false, true) == ServerRemoval::LeaveToGame);
}

TEST_CASE("a handed-away record matches only an actor of the base it kept", "[copylifetime]")
{
    REQUIRE(IsHandedAwayActor(0x23A8A, 0x23A8A));
    // The game gave the freed id to another kind of actor.
    REQUIRE_FALSE(IsHandedAwayActor(0x23A8A, 0x1B1DB));
    REQUIRE_FALSE(IsHandedAwayActor(0x23A8A, 0));
    // A record made after the actor was freed kept no base: it matches nothing, not every actor on that id.
    REQUIRE_FALSE(IsHandedAwayActor(0, 0x1B1DB));
    REQUIRE_FALSE(IsHandedAwayActor(0, 0));
}

TEST_CASE("a stand-in is made only for a foreign creature that is no temporary actor and no body kept here", "[copylifetime]")
{
    REQUIRE(StandsInForForeign(true, false, false));
    REQUIRE_FALSE(StandsInForForeign(false, false, false));
    // Its id is reused by the game once it is freed; the ghost record would then hide another actor.
    REQUIRE_FALSE(StandsInForForeign(true, true, false));
    REQUIRE_FALSE(StandsInForForeign(true, false, true));
}

TEST_CASE("removals asked for during a load are all kept, each once", "[copylifetime]")
{
    std::vector<LaterRemoval> kept;
    REQUIRE(QueueRemoval(kept, 0xFF000801, 0x100801));
    REQUIRE(QueueRemoval(kept, 0xFF000802, 0x100802));
    // The server's removal, then this client's own for the same copy: one entry.
    REQUIRE_FALSE(QueueRemoval(kept, 0xFF000801, 0x100801));
    // The same id on another actor is another removal.
    REQUIRE(QueueRemoval(kept, 0xFF000801, 0x200801));
    // Nothing had the id: its records still go.
    REQUIRE(QueueRemoval(kept, 0xFF000803, 0));
    REQUIRE(kept.size() == 4);

    // Many at once (a player leaving with every creature): none is dropped.
    for (uint32_t i = 0; i < 1000; ++i)
        QueueRemoval(kept, 0xFF100000 + i, 0x300000 + i);
    REQUIRE(kept.size() == 1004);
}

TEST_CASE("a kept removal touches only the actor its handle still names", "[copylifetime]")
{
    const LaterRemoval cKept{0xFF000801, 0x100801};
    REQUIRE(IsSameActorAfterLoad(cKept, true, 0xFF000801, false));
    // Freed during the load, or being freed.
    REQUIRE_FALSE(IsSameActorAfterLoad(cKept, false, 0, false));
    REQUIRE_FALSE(IsSameActorAfterLoad(cKept, true, 0xFF000801, true));
    REQUIRE_FALSE(IsSameActorAfterLoad(cKept, true, 0xFF000999, false));
    // Nothing had the id when it was asked for: no actor found later on that id is touched.
    REQUIRE_FALSE(IsSameActorAfterLoad(LaterRemoval{0xFF000801, 0}, true, 0xFF000801, false));
}

TEST_CASE("a copy nothing holds waits out a load, unless the waiting list is full", "[copylifetime]")
{
    REQUIRE_FALSE(WaitsOutLoad(true, 0, 256));
    REQUIRE(WaitsOutLoad(false, 0, 256));
    REQUIRE(WaitsOutLoad(false, 255, 256));
    REQUIRE_FALSE(WaitsOutLoad(false, 256, 256));
}

TEST_CASE("a queued switch-off of a hidden reference is done only while it is still hidden, loaded and on", "[copylifetime]")
{
    GhostFound found{};
    found.Actor = true;
    found.StillGhost = true;
    found.Has3D = true;
    REQUIRE(SwitchOffGhost(found));

    // Released before the frame started: never switched off, so left as it is.
    GhostFound released = found;
    released.StillGhost = false;
    REQUIRE_FALSE(SwitchOffGhost(released));

    GhostFound off = found;
    off.Disabled = true;
    REQUIRE_FALSE(SwitchOffGhost(off));

    GhostFound disposing = found;
    disposing.Has3D = false;
    REQUIRE_FALSE(SwitchOffGhost(disposing));

    GhostFound gone = found;
    gone.Actor = false;
    REQUIRE_FALSE(SwitchOffGhost(gone));

    GhostFound exiting = found;
    exiting.Exiting = true;
    REQUIRE_FALSE(SwitchOffGhost(exiting));
}

TEST_CASE("a waiting copy's move is logged once, and again after each load", "[copylifetime]")
{
    REQUIRE(LogsWaitingMove(false, 0, 7));
    REQUIRE_FALSE(LogsWaitingMove(true, 7, 7));
    REQUIRE(LogsWaitingMove(true, 7, 8));
}
