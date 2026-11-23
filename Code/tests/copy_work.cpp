#include <TiltedCore/Stl.hpp>

#include <catch2/catch.hpp>

#include <CopyWork.h>
#include <Services/CopyWorkQueue.h>

using namespace CopyWork;

namespace
{
// A copy found as the work was decided: no loading screen since, still a copy with its 3D, the decision still live.
Found Unchanged(const uint32_t aLoadingScreens = 4) noexcept
{
    Found found{};
    found.LoadingScreens = aLoadingScreens;
    found.Actor = true;
    found.Copy = true;
    found.Live = true;
    return found;
}
} // namespace

TEST_CASE("Work on a copy is done only under what it was decided under", "[copy_work]")
{
    const Stamp cSeat{7, 4};
    CHECK(Check(cSeat, Unchanged(), false) == Verdict::kDo);

    // The game exiting: nothing is touched.
    Found exiting = Unchanged();
    exiting.Exiting = true;
    CHECK(Check(cSeat, exiting, false) == Verdict::kDrop);

    // The actor gone, made again under the same form id (another handle), or without its 3D.
    Found gone = Unchanged();
    gone.Actor = false;
    CHECK(Check(cSeat, gone, false) == Verdict::kDrop);

    // Taken over by this game meanwhile: its own AI has it now.
    Found taken = Unchanged();
    taken.Copy = false;
    CHECK(Check(cSeat, taken, false) == Verdict::kDrop);

    // Withdrawn: a new owner's word, a new horse, or the ride or seat ended.
    Found withdrawn = Unchanged();
    withdrawn.Live = false;
    CHECK(Check(cSeat, withdrawn, false) == Verdict::kDrop);
}

TEST_CASE("A seat queued as a load starts is dropped", "[copy_work]")
{
    const Stamp cSeat{7, 4};

    // The loading screen is up when the work comes up.
    Found loading = Unchanged();
    loading.Loading = true;
    CHECK(Check(cSeat, loading, false) == Verdict::kDrop);

    // ...or it came and went between the decision and the frame.
    CHECK(Check(cSeat, Unchanged(5), false) == Verdict::kDrop);
}

TEST_CASE("Taking a copy off or out again waits out a load and runs after a disconnect", "[copy_work]")
{
    // An undo of ride 9, while ride 9 is the one in place.
    const Stamp cUndo{0, 4, 9};
    Found inPlace = Unchanged();
    inPlace.InPlace = 9;
    CHECK(Check(cUndo, inPlace, false) == Verdict::kDo);
    // A loading screen since is no reason to leave it on its horse or in its seat.
    Found later = Unchanged(6);
    later.InPlace = 9;
    CHECK(Check(cUndo, later, false) == Verdict::kDo);

    Found loading = inPlace;
    loading.Loading = true;
    CHECK(Check(cUndo, loading, false) == Verdict::kHold);
    loading.WaitedMs = kHoldUndoMs;
    CHECK(Check(cUndo, loading, false) == Verdict::kDrop);

    // Taken over by this game while it climbed off: left to its own AI.
    Found taken = inPlace;
    taken.Copy = false;
    CHECK(Check(cUndo, taken, false) == Verdict::kDrop);
    // After a disconnect every actor is this game's again, and what this game put on a horse is taken off it.
    CHECK(Check(cUndo, taken, true) == Verdict::kDo);
    // Not one deleted meanwhile, nor while the game exits.
    taken.Actor = false;
    CHECK(Check(cUndo, taken, true) == Verdict::kDrop);
    Found exiting = inPlace;
    exiting.Exiting = true;
    CHECK(Check(cUndo, exiting, true) == Verdict::kDrop);
}

TEST_CASE("An undo takes away only the seat it was for", "[copy_work]")
{
    const Stamp cUndoRide9{0, 4, 9};
    // A later ride (10) is in place now: getting off ride 9 leaves it alone.
    Found later = Unchanged();
    later.InPlace = 10;
    CHECK(Check(cUndoRide9, later, false) == Verdict::kDrop);
    CHECK(Check(cUndoRide9, later, true) == Verdict::kDrop);
    // Nothing of this game's in place (the seat never took, or was taken away already).
    CHECK(Check(cUndoRide9, Unchanged(), false) == Verdict::kDrop);
    // An undo naming no seat is never done.
    Found zero = Unchanged();
    CHECK(Check(Stamp{0, 4, 0}, zero, false) == Verdict::kDrop);

    Tickets tickets;
    tickets.Place(12, 9);
    CHECK(tickets.InPlace(12) == 9);
    tickets.Place(12, 10);
    CHECK(tickets.InPlace(12) == 10);
    tickets.Place(12, 0);
    CHECK(tickets.InPlace(12) == 0);
    CHECK(tickets.InPlace(13) == 0);
}

TEST_CASE("Work queued before an owner or target change is dropped when it comes up", "[copy_work]")
{
    // The queue as a service uses it: a seat decided under ticket A, then the copy's owner changes (or its word names another seat)
    // before the next frame, which issues ticket B.
    struct Work
    {
        uint32_t Key{};
        Stamp What{};
    };
    CopyWorkQueue<Work, int> queue;
    const uint32_t cA = queue.Issue(12);
    queue.Push({12, {cA, 4, 0}});
    const uint32_t cB = queue.Issue(12);
    CHECK(cB != cA);
    queue.Push({12, {cB, 4, 0}});

    TiltedPhoques::Vector<Verdict> verdicts;
    for (const Work& cWork : queue.TakeWork())
    {
        Found found = Unchanged();
        found.Live = queue.Live(cWork.Key, cWork.What.Ticket);
        verdicts.push_back(Check(cWork.What, found, false));
    }
    REQUIRE(verdicts.size() == 2);
    CHECK(verdicts[0] == Verdict::kDrop);
    CHECK(verdicts[1] == Verdict::kDo);

    // The copy became this game's: everything decided for it goes.
    const uint32_t cC = queue.Issue(12);
    queue.Push({12, {cC, 4, 0}});
    queue.Retire(12);
    for (const Work& cWork : queue.TakeWork())
    {
        Found found = Unchanged();
        found.Live = queue.Live(cWork.Key, cWork.What.Ticket);
        CHECK(Check(cWork.What, found, false) == Verdict::kDrop);
    }
}

TEST_CASE("Native work waits out a load and a moment after it", "[copy_work]")
{
    LoadGate gate;
    // The first pass knows no loading screen count yet: it waits too.
    CHECK_FALSE(Settled(gate, false, false, 3, 1000));
    CHECK(Settled(gate, false, false, 3, 1000 + kSettleAfterLoadMs));
    // A loading screen up: not while it lasts, and not just after.
    CHECK_FALSE(Settled(gate, false, true, 4, 5000));
    CHECK_FALSE(Settled(gate, false, false, 4, 5100));
    CHECK(Settled(gate, false, false, 4, 5000 + kSettleAfterLoadMs));
    // A load that came and went between two passes, never seen up.
    CHECK_FALSE(Settled(gate, false, false, 5, 9000));
    CHECK(Settled(gate, false, false, 5, 9000 + kSettleAfterLoadMs));
    // Never while the game exits.
    CHECK_FALSE(Settled(gate, true, false, 5, 20000));
}

TEST_CASE("A new decision withdraws the last one's work, and a retired one withdraws all of it", "[copy_work]")
{
    Tickets tickets;
    const uint32_t cFirst = tickets.Issue(12);
    CHECK(tickets.Live(12, cFirst));
    CHECK_FALSE(tickets.Live(13, cFirst));

    // A refresh from the rider's new owner, a disconnect during a climb.
    const uint32_t cSecond = tickets.Issue(12);
    CHECK(cSecond != cFirst);
    CHECK_FALSE(tickets.Live(12, cFirst));
    CHECK(tickets.Live(12, cSecond));

    tickets.Retire(12);
    CHECK_FALSE(tickets.Live(12, cSecond));
    // An undo carries no ticket and is never live as a decision.
    CHECK_FALSE(tickets.Live(12, 0));

    const uint32_t cOther = tickets.Issue(30);
    tickets.Clear();
    CHECK_FALSE(tickets.Live(30, cOther));
}

TEST_CASE("Work held over a loading screen keeps its place ahead of later work", "[copy_work]")
{
    CopyWorkQueue<int, int> queue;
    queue.Push(1);
    queue.Push(2);
    auto work = queue.TakeWork();
    REQUIRE(work.size() == 2);

    // 2 waits; 3 is queued meanwhile.
    queue.Push(3);
    TiltedPhoques::Vector<int> held{2};
    queue.Hold(held);
    const auto cNext = queue.TakeWork();
    REQUIRE(cNext.size() == 2);
    CHECK(cNext[0] == 2);
    CHECK(cNext[1] == 3);

    queue.Give(9);
    const auto cDone = queue.TakeDone();
    REQUIRE(cDone.size() == 1);
    CHECK(cDone[0] == 9);
    CHECK(queue.TakeDone().empty());
}

TEST_CASE("Queued work looks at the load state again right before the engine call", "[copy_work]")
{
    const Stamp cSeat{7, 4};
    const Stamp cUndo{0, 4, 9};
    const Moment cBefore{false, false, 4};

    // Nothing changed between the first look and the call: it goes ahead.
    CHECK(Recheck(cSeat, cBefore, cBefore) == Verdict::kDo);
    CHECK(Recheck(cUndo, cBefore, cBefore) == Verdict::kDo);

    // A loading screen queued while the actor was looked up: the decision is dropped, the undo waits for the next frame.
    const Moment cQueued{false, false, 5};
    CHECK(Recheck(cSeat, cBefore, cQueued) == Verdict::kDrop);
    CHECK(Recheck(cUndo, cBefore, cQueued) == Verdict::kHold);
    const Moment cUp{false, true, 5};
    CHECK(Recheck(cSeat, cBefore, cUp) == Verdict::kDrop);
    CHECK(Recheck(cUndo, cBefore, cUp) == Verdict::kHold);

    // A first look taken during a load never lets the call through.
    const Moment cLoadingBefore{false, true, 4};
    CHECK(Recheck(cUndo, cLoadingBefore, cBefore) == Verdict::kHold);

    // The exit drops both.
    const Moment cExiting{true, false, 4};
    CHECK(Recheck(cSeat, cBefore, cExiting) == Verdict::kDrop);
    CHECK(Recheck(cUndo, cBefore, cExiting) == Verdict::kDrop);
}

namespace
{
// One frame of the rider executor for a copy handed over to be deleted (MountService::RunQueuedWork): Check under the first look,
// Recheck right before the call, then StepForDelete.
struct DeleteFrame
{
    Moment Before{false, false, 4};
    Moment AtCall{false, false, 4};
    bool There = true;
    bool Has3D = true;
    bool OnHorse = true;
    uint32_t InPlace = 9;
    uint64_t WaitedMs{};
};

DeleteStep RunDeleteFrame(const DeleteFrame& acFrame) noexcept
{
    const Stamp cUndo{0, 4, 9};
    Found found{};
    found.Loading = acFrame.Before.Loading;
    found.LoadingScreens = acFrame.Before.LoadingScreens;
    // Nothing is looked up during a load.
    const bool cLooked = !acFrame.Before.Loading;
    found.Actor = cLooked && acFrame.There && acFrame.Has3D;
    found.InPlace = acFrame.InPlace;
    found.WaitedMs = acFrame.WaitedMs;
    const Verdict cFirst = Check(cUndo, found, true);
    const Verdict cVerdict = cFirst == Verdict::kDo ? Recheck(cUndo, acFrame.Before, acFrame.AtCall) : cFirst;
    const bool cThere = cLooked && acFrame.There;
    const bool cRidden = cThere && (acFrame.OnHorse || acFrame.InPlace == cUndo.Undoes);
    return StepForDelete(cFirst, cVerdict, found, cThere, cRidden);
}
} // namespace

TEST_CASE("A ridden copy is never deleted before it is taken off its horse", "[copy_work]")
{
    // Loaded, on its horse: taken off, then deleted.
    CHECK(RunDeleteFrame({}) == DeleteStep::kReleaseThenDelete);

    // A load longer than an undo's hold: it still waits, past kHoldUndoMs.
    DeleteFrame loading{};
    loading.Before = {false, true, 5};
    loading.AtCall = loading.Before;
    loading.WaitedMs = kHoldUndoMs + 10000;
    CHECK(RunDeleteFrame(loading) == DeleteStep::kWait);

    // The load over, but the copy has no 3D (its cell unloaded): still on its horse, so it waits.
    DeleteFrame unloaded{};
    unloaded.Before = {false, false, 5};
    unloaded.AtCall = unloaded.Before;
    unloaded.Has3D = false;
    unloaded.WaitedMs = kHoldUndoMs + 20000;
    CHECK(RunDeleteFrame(unloaded) == DeleteStep::kWait);
    // ...and with only the seat this game put in place left (not on a horse by the game's word), the same.
    unloaded.OnHorse = false;
    CHECK(RunDeleteFrame(unloaded) == DeleteStep::kWait);

    // Its 3D back: taken off and deleted.
    DeleteFrame back = unloaded;
    back.Has3D = true;
    back.OnHorse = true;
    CHECK(RunDeleteFrame(back) == DeleteStep::kReleaseThenDelete);

    // A load queued between the first look and the call: it waits for the next frame, not deleted on its horse.
    DeleteFrame queued{};
    queued.AtCall = {false, false, 5};
    CHECK(RunDeleteFrame(queued) == DeleteStep::kWait);
}

TEST_CASE("A copy handed over to be deleted that is not on a horse, or gone, is not held", "[copy_work]")
{
    // Off its horse already (another undo took it off), no 3D: nothing to take it off, so deleted as it is.
    DeleteFrame off{};
    off.Has3D = false;
    off.OnHorse = false;
    off.InPlace = 0;
    CHECK(RunDeleteFrame(off) == DeleteStep::kDelete);
    // A later ride of this game in place, not on a horse by the game's word: the undo is not for it, and it is deleted.
    off.InPlace = 10;
    CHECK(RunDeleteFrame(off) == DeleteStep::kDelete);

    // Freed by the game (its handle names nothing): nothing to do.
    DeleteFrame gone{};
    gone.There = false;
    CHECK(RunDeleteFrame(gone) == DeleteStep::kNothing);

    // The game exiting at the call: nothing is touched.
    DeleteFrame exiting{};
    exiting.AtCall = {true, false, 4};
    CHECK(RunDeleteFrame(exiting) == DeleteStep::kNothing);
    Found exitingFound{};
    exitingFound.Exiting = true;
    CHECK(StepForDelete(Verdict::kDrop, Verdict::kDrop, exitingFound, true, true) == DeleteStep::kNothing);
}

TEST_CASE("Furniture and horse work rechecked mid-batch drop or hold as their kind says", "[copy_work]")
{
    // A batch of a stand (undo), a seat (decision) and another stand, with a load queued after the first item ran.
    const Moment cStart{false, false, 4};
    const Moment cAfterFirst{false, false, 5};
    CHECK(Recheck(Stamp{0, 4, 3}, cStart, cStart) == Verdict::kDo);
    // The next items read their own first look after the load was queued: Check already drops or holds them.
    Found seat{};
    seat.LoadingScreens = cAfterFirst.LoadingScreens;
    seat.Actor = seat.Copy = seat.Live = true;
    CHECK(Check(Stamp{8, 4}, seat, false) == Verdict::kDrop);
    // One whose first look came just before the load was queued is caught by the second.
    CHECK(Recheck(Stamp{8, 4}, cStart, cAfterFirst) == Verdict::kDrop);
    CHECK(Recheck(Stamp{0, 4, 3}, cStart, cAfterFirst) == Verdict::kHold);
}
