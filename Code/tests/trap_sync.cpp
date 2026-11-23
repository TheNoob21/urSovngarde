#include <catch2/catch.hpp>

#include <TrapSync.h>

using namespace TrapSync;
using namespace std::chrono_literals;

namespace
{
constexpr uint32_t cPlate = 0x10A3F0;
constexpr uint32_t cTrap = 0x10A3F2;
const auto cStart = std::chrono::steady_clock::time_point{} + 1h;
} // namespace

TEST_CASE("a trap one player sets off fires in the other players' games", "[traps]")
{
    SECTION("the pressure plates, tripwires and linkers fire it, not the Dwemer button a player presses")
    {
        CHECK(IsTriggerBase(0x0001D9F9));       // TrapStonePressurePlate01
        CHECK(IsTriggerBase(0x00022F54));       // TrapTripwire01
        CHECK(IsTriggerBase(0x00079986));       // TrapLinker
        CHECK(IsTriggerBase(0x000AE4E0));       // TrapTriggerHinge
        CHECK_FALSE(IsTriggerBase(0x0001BA5A)); // DweButton01
        CHECK_FALSE(IsTriggerBase(0x0001CDD0)); // TrapBladeSwinging01, a trap, not a trigger
        CHECK_FALSE(IsTriggerBase(0x0201D9F9)); // the same id in another plugin
    }

    Recent recent;

    SECTION("is sent by the game whose actor stepped on the trigger, within a second")
    {
        CHECK_FALSE(recent.IsTouchedHere(cPlate, cStart));
        recent.Touched(cPlate, cStart);
        CHECK(recent.IsTouchedHere(cPlate, cStart + 500ms));
        CHECK_FALSE(recent.IsTouchedHere(cPlate, cStart + 1s));
        CHECK_FALSE(recent.IsTouchedHere(0x10A3F1, cStart + 500ms));
    }

    SECTION("is not fired again for another game when it fired here less than 2 s before")
    {
        recent.Fired(cTrap, cStart);
        CHECK(recent.IsFiredRecently(cTrap, cStart + 1900ms));
        CHECK_FALSE(recent.IsFiredRecently(cTrap, cStart + 2s));
    }

    SECTION("is not fired again when its trigger, fired here from the other game, already fired the traps it is parent of")
    {
        CHECK_FALSE(recent.IsReplayed(cPlate, cStart));
        recent.Replayed(cPlate, cStart);
        CHECK(recent.IsReplayed(cPlate, cStart + 1500ms));
        CHECK_FALSE(recent.IsReplayed(cPlate, cStart + 2s));
        CHECK_FALSE(recent.IsFiredRecently(cPlate, cStart + 1500ms));
    }

    SECTION("keeps only the triggers and traps used lately")
    {
        recent.Touched(cPlate, cStart);
        recent.Fired(cTrap, cStart);
        recent.Replayed(cPlate, cStart);
        recent.Touched(0x10A3F5, cStart + 1500ms);
        recent.Forget(cStart + 1500ms);
        CHECK(recent.Size() == 3);
        recent.Forget(cStart + 5s);
        CHECK(recent.Size() == 0);
    }

    SECTION("the server relays at most eight a second from one player")
    {
        RateLimit limit;
        for (int i = 0; i < 8; ++i)
            CHECK(limit.Allow(cStart + std::chrono::milliseconds(i)));
        CHECK_FALSE(limit.Allow(cStart + 100ms));
        CHECK(limit.Allow(cStart + 1s));
    }
}

TEST_CASE("one step on a trigger fires its trap once in a game that sees the stepper as a copy", "[traps]")
{
    SECTION("the copy steps first and the word comes later than 2 s: the word is dropped")
    {
        Ledger ledger;
        REQUIRE(ledger.CopyFire(cTrap, cStart));
        REQUIRE(ledger.TakeCopyFire(cTrap, cStart + 5s));
        // Paired: a second word is a new step.
        REQUIRE_FALSE(ledger.TakeCopyFire(cTrap, cStart + 6s));
    }

    SECTION("the word comes first and the copy steps later: the copy's firing is dropped")
    {
        Ledger ledger;
        REQUIRE_FALSE(ledger.TakeCopyFire(cTrap, cStart));
        ledger.WordFired(cTrap, cStart);
        REQUIRE_FALSE(ledger.CopyFire(cTrap, cStart + 3s));
        // Paired: the copy's next step fires again.
        REQUIRE(ledger.CopyFire(cTrap, cStart + 4s));
    }

    SECTION("two steps pair two by two, in either order")
    {
        Ledger ledger;
        ledger.WordFired(cTrap, cStart);
        ledger.WordFired(cTrap, cStart + 1s);
        REQUIRE_FALSE(ledger.CopyFire(cTrap, cStart + 2s));
        REQUIRE_FALSE(ledger.CopyFire(cTrap, cStart + 3s));
        REQUIRE(ledger.CopyFire(cTrap, cStart + 4s));
    }

    SECTION("an unpaired firing no longer counts after the pairing window")
    {
        Ledger ledger;
        REQUIRE(ledger.CopyFire(cTrap, cStart));
        REQUIRE_FALSE(ledger.TakeCopyFire(cTrap, cStart + kPairWindow));

        ledger.WordFired(cTrap, cStart);
        REQUIRE(ledger.CopyFire(cTrap, cStart + kPairWindow));
    }

    SECTION("each trap pairs on its own")
    {
        Ledger ledger;
        ledger.WordFired(cTrap, cStart);
        REQUIRE(ledger.CopyFire(cTrap + 1, cStart + 1s));
        REQUIRE_FALSE(ledger.CopyFire(cTrap, cStart + 1s));
    }

    SECTION("a stuck trigger keeps only a few unpaired firings, and all are forgotten in time")
    {
        Ledger ledger;
        for (int i = 0; i < 50; ++i)
            REQUIRE(ledger.CopyFire(cTrap, cStart + std::chrono::milliseconds(i)));
        REQUIRE(ledger.Size(cStart + 1s) == kMaxUnpaired);
        REQUIRE(ledger.Size(cStart + 1h) == 0);
    }
}

TEST_CASE("the ledger tells who set a trigger off just now", "[traps]")
{
    Ledger ledger;
    REQUIRE(ledger.LastToucher(cPlate, cStart) == Ledger::Toucher::kNone);

    ledger.Touched(cPlate, true, cStart);
    REQUIRE(ledger.LastToucher(cPlate, cStart + 500ms) == Ledger::Toucher::kRemote);
    REQUIRE(ledger.LastToucher(cPlate, cStart + kTouchWindow) == Ledger::Toucher::kNone);

    // This game's actor on it too: its firing is this game's own, never dropped.
    ledger.Touched(cPlate, true, cStart + 2s);
    ledger.Touched(cPlate, false, cStart + 2s);
    REQUIRE(ledger.LastToucher(cPlate, cStart + 2s) == Ledger::Toucher::kLocal);

    ledger.Clear();
    REQUIRE(ledger.LastToucher(cPlate, cStart + 2s) == Ledger::Toucher::kNone);
}

TEST_CASE("firing for another game's word is marked for the hook only while it runs", "[traps]")
{
    REQUIRE_FALSE(t_firingForWord);
    {
        ScopedFiringForWord outer;
        REQUIRE(t_firingForWord);
        {
            ScopedFiringForWord inner;
            REQUIRE(t_firingForWord);
        }
        REQUIRE(t_firingForWord);
    }
    REQUIRE_FALSE(t_firingForWord);
}
