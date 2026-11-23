#include <catch2/catch.hpp>

#include <Systems/RaiseClaim.h>

#include <chrono>
#include <initializer_list>
#include <thread>

using namespace RaiseClaim;

namespace
{
// This player raising a dead body another game runs.
Attempt RaisingACopy()
{
    Attempt cast{};
    cast.Connected = true;
    cast.CasterIsPlayer = true;
    cast.TargetIsCopy = true;
    cast.Reanimate = true;
    cast.Dead = true;
    return cast;
}
} // namespace

TEST_CASE("this player's raise of a body another game runs waits for the body", "[raise]")
{
    CHECK(AskFirst(RaisingACopy()));

    // Offline, cast by someone else, on a body this game runs, another effect, a living target: not held for the body.
    for (bool Attempt::* pGate : {&Attempt::Connected, &Attempt::CasterIsPlayer, &Attempt::TargetIsCopy, &Attempt::Reanimate, &Attempt::Dead})
    {
        Attempt cast = RaisingACopy();
        cast.*pGate = false;
        CHECK_FALSE(AskFirst(cast));
    }

    // A raise being sent to the game that runs the body is not asked for again.
    Attempt again = RaisingACopy();
    again.ToOwner = true;
    CHECK_FALSE(AskFirst(again));
}

TEST_CASE("a raise is made once: here when the body comes, in the game that runs it when it does not", "[raise]")
{
    CHECK(OnHandedOver(true) == Then::RaiseHere);
    CHECK(OnNotHandedOver(true) == Then::ToOwner);

    // Revived or raised meanwhile: nothing is left to make, either way.
    CHECK(OnHandedOver(false) == Then::Nothing);
    CHECK(OnNotHandedOver(false) == Then::Nothing);
}

namespace
{
using namespace std::chrono_literals;

const Clock::time_point kT0{std::chrono::hours(1)};

// The body as the main thread finds it when the raise was decided for it.
Found AsDecided(const Ready<int>& acReady, const uint32_t aSession)
{
    Found found{};
    found.Session = aSession;
    found.SameBody = true;
    found.Here = acReady.Make == Then::RaiseHere;
    found.Dead = true;
    found.Decided = true;
    return found;
}
} // namespace

TEST_CASE("a raise is made only on the main thread's turn, whichever thread decided it", "[raise]")
{
    Raises<int> raises;
    const uint32_t cSession = raises.Session();
    const Target cBody{7, 0x100007, cSession};

    // Refused claim, grant and timeout only decide; nothing is made until Take.
    CHECK(raises.NotAskedFor(cBody, true, kT0, 1) == Then::ToOwner);
    REQUIRE(raises.Wait(Target{8, 0x100008, cSession}, kT0, 2));
    CHECK(raises.HandedOver(8, true, kT0 + 1s) == Then::RaiseHere);
    REQUIRE(raises.Wait(Target{9, 0x100009, cSession}, kT0, 3));
    CHECK(raises.Expire(kT0 + kWaitForBody, [](uint32_t) { return true; }) == 1);

    auto ready = raises.Take();
    REQUIRE(ready.size() == 3);
    CHECK(ready[0].Make == Then::ToOwner);
    CHECK(ready[1].Make == Then::RaiseHere);
    CHECK(ready[2].Make == Then::ToOwner);
    CHECK(raises.Take().empty());

    // Decided on an update thread while the main thread takes: every one reaches the main thread once.
    constexpr int cCount = 200;
    std::thread update(
        [&]
        {
            for (int i = 0; i < cCount; ++i)
                raises.NotAskedFor(Target{static_cast<uint32_t>(100 + i), 1, cSession}, true, kT0, i);
        });
    const auto cMain = std::this_thread::get_id();
    int made = 0;
    bool allOnMain = true;
    while (made < cCount)
    {
        for (const auto& cRaise : raises.Take())
        {
            allOnMain = allOnMain && std::this_thread::get_id() == cMain && StillToMake(cRaise, AsDecided(cRaise, cSession), kT0);
            ++made;
        }
        std::this_thread::yield();
    }
    update.join();
    CHECK(made == cCount);
    CHECK(allOnMain);
}

TEST_CASE("a raise is not made after a disconnect, whatever stage it reached", "[raise]")
{
    Raises<int> raises;
    const uint32_t cOld = raises.Session();
    REQUIRE(raises.Wait(Target{7, 0x100007, cOld}, kT0, 1));
    raises.NotAskedFor(Target{8, 0x100008, cOld}, true, kT0, 2);
    const auto cTakenBefore = raises.Take();
    REQUIRE(cTakenBefore.size() == 1);
    REQUIRE(raises.Wait(Target{9, 0x100009, cOld}, kT0, 3));

    raises.Clear();
    const uint32_t cNew = raises.Session();
    CHECK(cNew != cOld);

    // Waiting: the grant after the reconnect finds nothing; nor does the timeout.
    CHECK(raises.HandedOver(9, true, kT0 + 1s) == Then::Nothing);
    CHECK(raises.Expire(kT0 + kWaitForBody, [](uint32_t) { return true; }) == 0);
    CHECK(raises.Take().empty());
    // Taken already by the main thread: refused there.
    CHECK_FALSE(StillToMake(cTakenBefore[0], AsDecided(cTakenBefore[0], cNew), kT0));
    // Queued by the hook before the disconnect and asked for after it: refused at every step.
    CHECK_FALSE(raises.Wait(Target{10, 0x10000A, cOld}, kT0, 4));
    CHECK(raises.NotAskedFor(Target{10, 0x10000A, cOld}, true, kT0, 4) == Then::Nothing);
    CHECK_FALSE(raises.Renew(Target{7, 0x100007, cOld}, kT0, 5));
    CHECK(raises.Take().empty());
}

TEST_CASE("a grant that comes after the raise stopped waiting raises nothing", "[raise]")
{
    Raises<int> raises;
    const uint32_t cSession = raises.Session();
    REQUIRE(raises.Wait(Target{7, 0x100007, cSession}, kT0, 1));
    // The timeout has not run yet (a long frame), and the grant comes late.
    CHECK(raises.HandedOver(7, true, kT0 + kWaitForBody) == Then::Nothing);
    CHECK(raises.Take().empty());
    // Nor is it sent to the game that runs it afterwards: it no longer waits.
    CHECK(raises.Expire(kT0 + 10s, [](uint32_t) { return true; }) == 0);

    // In time, and revived meanwhile: nothing either.
    REQUIRE(raises.Wait(Target{8, 0x100008, cSession}, kT0, 2));
    CHECK(raises.HandedOver(8, false, kT0 + 1s) == Then::Nothing);
    CHECK(raises.Take().empty());
}

TEST_CASE("a raise decided on is dropped when its body is gone, replaced, changed hands or comes too late", "[raise]")
{
    Raises<int> raises;
    const uint32_t cSession = raises.Session();
    REQUIRE(raises.Wait(Target{7, 0x100007, cSession}, kT0, 1));
    REQUIRE(raises.HandedOver(7, true, kT0 + 1s) == Then::RaiseHere);
    const auto cReady = raises.Take();
    REQUIRE(cReady.size() == 1);
    const Ready<int>& cRaise = cReady[0];
    CHECK(cRaise.Body.Handle == 0x100007);
    CHECK(StillToMake(cRaise, AsDecided(cRaise, cSession), kT0 + 1s));

    Found gone = AsDecided(cRaise, cSession);
    gone.SameBody = false; // the handle no longer gives the body (freed, or a new reference under the id)
    CHECK_FALSE(StillToMake(cRaise, gone, kT0 + 1s));
    Found lost = AsDecided(cRaise, cSession);
    lost.Here = false; // another game runs it again
    CHECK_FALSE(StillToMake(cRaise, lost, kT0 + 1s));
    Found revived = AsDecided(cRaise, cSession);
    revived.Dead = false;
    CHECK_FALSE(StillToMake(cRaise, revived, kT0 + 1s));
    Found loading = AsDecided(cRaise, cSession);
    loading.Loading = true;
    CHECK_FALSE(StillToMake(cRaise, loading, kT0 + 1s));
    CHECK_FALSE(StillToMake(cRaise, AsDecided(cRaise, cSession), kT0 + 1s + kMakeWithin));

    // A raise sent to the game that runs the body is for a copy: not once this game runs the body.
    raises.NotAskedFor(Target{8, 0x100008, cSession}, true, kT0, 2);
    const auto cToOwner = raises.Take();
    REQUIRE(cToOwner.size() == 1);
    Found taken = AsDecided(cToOwner[0], cSession);
    taken.Here = true;
    CHECK_FALSE(StillToMake(cToOwner[0], taken, kT0));

    // The body changes hands before the main thread's turn: withdrawn.
    raises.NotAskedFor(Target{9, 0x100009, cSession}, true, kT0, 3);
    raises.NotAskedFor(Target{10, 0x10000A, cSession}, true, kT0, 4);
    raises.Withdraw(9);
    const auto cLeft = raises.Take();
    REQUIRE(cLeft.size() == 1);
    CHECK(cLeft[0].Body.ServerId == 10);
}

TEST_CASE("a raise whose body was replaced before the update asked for it asks for no body", "[raise]")
{
    Raises<int> raises;
    const uint32_t cSession = raises.Session();
    // As AskToRaise finds it: the hook's handle, then the copy under the form id.
    CHECK(MayAsk(true, true));
    // The hook's reference is gone and another copy took its form id: nothing is asked for, and nothing is made on the new one.
    CHECK_FALSE(MayAsk(false, true));
    const Target cReplaced{0, 0x100007, cSession};
    CHECK(raises.NotAskedFor(cReplaced, false, kT0, 1) == Then::Nothing);
    CHECK_FALSE(raises.Waits());
    CHECK(raises.Take().empty());
    // Still the same reference, but no copy is bound to its form id: sent to the game that runs it, for that reference only.
    CHECK_FALSE(MayAsk(true, false));
    CHECK(raises.NotAskedFor(cReplaced, true, kT0, 2) == Then::ToOwner);
    CHECK_FALSE(raises.Waits());
}

TEST_CASE("a raise the main thread has taken is not made once its body changed hands", "[raise]")
{
    Raises<int> raises;
    const uint32_t cSession = raises.Session();
    REQUIRE(raises.Wait(Target{7, 0x100007, cSession}, kT0, 1));
    REQUIRE(raises.HandedOver(7, true, kT0) == Then::RaiseHere);
    raises.NotAskedFor(Target{8, 0x100008, cSession}, true, kT0, 2);
    raises.NotAskedFor(Target{9, 0x100009, cSession}, true, kT0, 3);
    const auto cTaken = raises.Take();
    REQUIRE(cTaken.size() == 3);

    // After Take, before the effect: body 7 goes to another game and back (here again), body 8 from one other game to another (still not
    // here). As the main thread finds them, each would pass every other check.
    raises.Withdraw(7);
    raises.Withdraw(8);
    for (size_t i = 0; i < cTaken.size(); ++i)
    {
        Found found = AsDecided(cTaken[i], cSession);
        found.Decided = raises.Decided(i);
        CHECK(StillToMake(cTaken[i], found, kT0) == (cTaken[i].Body.ServerId == 9));
    }

    // Withdrawn on another thread while the main thread works through the batch.
    raises.NotAskedFor(Target{10, 0x10000A, cSession}, true, kT0, 4);
    REQUIRE(raises.Take().size() == 1);
    CHECK(raises.Decided(0));
    std::thread update([&] { raises.Withdraw(10); });
    update.join();
    CHECK_FALSE(raises.Decided(0));

    // A disconnect after Take, and the next Take: nothing of the last batch is decided.
    raises.NotAskedFor(Target{11, 0x10000B, cSession}, true, kT0, 5);
    REQUIRE(raises.Take().size() == 1);
    raises.Clear();
    CHECK_FALSE(raises.Decided(0));
    CHECK(raises.Take().empty());
    CHECK_FALSE(raises.Decided(0));
}

TEST_CASE("a second cast on a body waited for replaces the first, but not on a replaced body", "[raise]")
{
    Raises<int> raises;
    const uint32_t cSession = raises.Session();
    REQUIRE(raises.Wait(Target{7, 0x100007, cSession}, kT0, 1));
    CHECK(raises.Renew(Target{7, 0x100007, cSession}, kT0 + 1s, 2));
    // Another reference now under the same server id: not the body waited for.
    CHECK_FALSE(raises.Renew(Target{7, 0x200007, cSession}, kT0 + 1s, 3));
    // Waited for no longer.
    CHECK_FALSE(raises.Renew(Target{7, 0x100007, cSession}, kT0 + kWaitForBody, 4));
    REQUIRE(raises.HandedOver(7, true, kT0 + 2s) == Then::RaiseHere);
    const auto cReady = raises.Take();
    REQUIRE(cReady.size() == 1);
    CHECK(cReady[0].Cast == 2);

    // Not dead when the wait runs out: nothing is sent.
    REQUIRE(raises.Wait(Target{8, 0x100008, cSession}, kT0, 5));
    CHECK(raises.Expire(kT0 + kWaitForBody, [](uint32_t) { return false; }) == 1);
    CHECK(raises.Take().empty());
}
