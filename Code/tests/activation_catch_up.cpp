#include <catch2/catch.hpp>

#include <ActivationCatchUp.h>
#include <ActivationLedger.h>
#include <SharedHomes.h>

#include <string>

namespace
{
// A cell id and nothing else: enough for the ledger, which asks the caller what is in range.
struct Place
{
    uint32_t Cell;
};

auto InCell(uint32_t aCell)
{
    return [aCell](const Place& acPlace)
    {
        return acPlace.Cell == aCell;
    };
}
} // namespace

TEST_CASE("a player whose game was not near a lever gets its uses as they come near, once", "[activation_catch_up]")
{
    ActivationLedger<Place> ledger;
    constexpr uint64_t cLever = 0x21513;
    constexpr uint64_t cRing = 0x4E2AE;

    SECTION("the uses the player missed come oldest first, in the order they happened")
    {
        ledger.Record(cRing, {1}, {"Owner"});
        ledger.Record(cLever, {1}, {"Owner"});
        ledger.Record(cRing, {1}, {"Owner"});
        CHECK(ledger.CatchUp("Brother", InCell(1)) == std::vector<uint64_t>{cRing, cLever, cRing});
    }

    SECTION("they come only once: a second visit, or a reconnect under the same name, gets nothing")
    {
        ledger.Record(cLever, {1}, {"Owner"});
        CHECK(ledger.CatchUp("Brother", InCell(1)).size() == 1);
        CHECK(ledger.CatchUp("Brother", InCell(1)).empty());
    }

    SECTION("the player who used it, and one it was passed on to live, already did it")
    {
        ledger.Record(cLever, {1}, {"Owner", "Brother"});
        CHECK(ledger.CatchUp("Owner", InCell(1)).empty());
        CHECK(ledger.CatchUp("Brother", InCell(1)).empty());
        CHECK(ledger.CatchUp("Third", InCell(1)).size() == 1);
    }

    SECTION("only the uses near the player now; the others wait for that player to come near them")
    {
        ledger.Record(cLever, {1}, {"Owner"});
        ledger.Record(cRing, {2}, {"Owner"});
        CHECK(ledger.CatchUp("Brother", InCell(2)) == std::vector<uint64_t>{cRing});
        CHECK(ledger.CatchUp("Brother", InCell(1)) == std::vector<uint64_t>{cLever});
    }

    SECTION("at most a message's worth at once, the rest at the next cell change")
    {
        for (size_t i = 0; i < ActivationLedger<Place>::kMaxCatchUp + 10; ++i)
            ledger.Record(cLever, {1}, {"Owner"});
        CHECK(ledger.CatchUp("Brother", InCell(1)).size() == ActivationLedger<Place>::kMaxCatchUp);
        CHECK(ledger.CatchUp("Brother", InCell(1)).size() == 10);
    }

    SECTION("the oldest uses go first once the ledger is full")
    {
        ledger.Record(cRing, {1}, {"Owner"});
        for (size_t i = 0; i < ActivationLedger<Place>::kMaxEntries; ++i)
            ledger.Record(cLever, {1}, {"Owner"});
        CHECK(ledger.Size() == ActivationLedger<Place>::kMaxEntries);
        const auto cRefs = ledger.CatchUp("Brother", InCell(1));
        CHECK(std::find(cRefs.begin(), cRefs.end(), cRing) == cRefs.end());
    }

    SECTION("a new session starts from the players' saves")
    {
        ledger.Record(cLever, {1}, {"Owner"});
        ledger.Clear();
        CHECK(ledger.CatchUp("Brother", InCell(1)).empty());
    }
}

TEST_CASE("only mechanisms are replayed for a late player, never a shrine or a trap trigger", "[activation_catch_up]")
{
    using ActivationCatchUp::IsMechanism;
    using ActivationCatchUp::Plugin;

    SECTION("levers, chains, pillars and the claw doors' rings and keyholes")
    {
        CHECK(IsMechanism(Plugin::Skyrim, 0x21513));     // NorLever01
        CHECK(IsMechanism(Plugin::Skyrim, 0x98665));     // NorPullChain01
        CHECK(IsMechanism(Plugin::Skyrim, 0x1717B));     // RuinsPuzzlePillar01
        CHECK(IsMechanism(Plugin::Skyrim, 0x4E2AE));     // PuzzleDoorSmallWheel01
        CHECK(IsMechanism(Plugin::Skyrim, 0x4E2B2));     // PuzzleDoorKeyHole01
        CHECK(IsMechanism(Plugin::Dawnguard, 0x7F4D));   // DLC1FalmerPullChain01
        CHECK(IsMechanism(Plugin::Dragonborn, 0x38D44)); // DLC2SV01FullClawKeyHole
    }

    SECTION("a shrine, Dibella's altar, a standing stone's base, a trap trigger or the Dwemer button are not")
    {
        CHECK_FALSE(IsMechanism(Plugin::Skyrim, 0x6C9F1)); // T01DibellasAltarActivator (TempleBlessingScript)
        CHECK_FALSE(IsMechanism(Plugin::Skyrim, 0x1D9F9)); // TrapStonePressurePlate01
        CHECK_FALSE(IsMechanism(Plugin::Skyrim, 0x22F54)); // TrapTripwire01
        CHECK_FALSE(IsMechanism(Plugin::Skyrim, 0x1BA5A)); // DweButton01 (TrapTriggerBase)
    }

    SECTION("an id is read within its own plugin only")
    {
        CHECK_FALSE(IsMechanism(Plugin::Dawnguard, 0x21513));
        CHECK_FALSE(IsMechanism(Plugin::Other, 0x21513));
    }
}

TEST_CASE("the uses are replayed in order, one at a time, each once its cell is loaded here", "[activation_catch_up]")
{
    using Clock = std::chrono::steady_clock;
    ActivationCatchUp::Pacer pacer;
    const auto cStart = Clock::now();
    using ActivationCatchUp::Readiness;
    const auto cAlways = [](uint32_t)
    {
        return Readiness::Go;
    };

    SECTION("one use, then the next only after the gap")
    {
        pacer.Add(1, cStart);
        pacer.Add(2, cStart);
        CHECK(pacer.Next(cStart, cAlways).FormId == 1);
        CHECK(pacer.Next(cStart + std::chrono::seconds(1), cAlways).FormId == 0);
        CHECK(pacer.Next(cStart + ActivationCatchUp::kGap, cAlways).FormId == 2);
        CHECK(pacer.Size() == 0);
    }

    SECTION("a use whose cell is not loaded holds back the ones after it")
    {
        pacer.Add(1, cStart);
        pacer.Add(2, cStart);
        const auto cOnlyTwo = [](uint32_t aFormId)
        {
            return aFormId == 2 ? Readiness::Go : Readiness::Wait;
        };
        CHECK(pacer.Next(cStart, cOnlyTwo).FormId == 0);
        CHECK(pacer.Next(cStart + std::chrono::seconds(5), cAlways).FormId == 1);
    }

    SECTION("one that never loads is given up after a while, and the next waits its own time")
    {
        pacer.Add(1, cStart);
        pacer.Add(2, cStart);
        const auto cNever = [](uint32_t)
        {
            return Readiness::Wait;
        };
        const auto cGiveUp = pacer.Next(cStart + ActivationCatchUp::kWait, cNever);
        CHECK(cGiveUp.FormId == 1);
        CHECK(cGiveUp.Dropped);
        CHECK(pacer.Next(cStart + ActivationCatchUp::kWait + std::chrono::seconds(1), cNever).FormId == 0);
        CHECK(pacer.Size() == 1);
    }

    SECTION("a use that is no mechanism is passed over at once, without a gap")
    {
        pacer.Add(1, cStart);
        pacer.Add(2, cStart);
        pacer.Add(3, cStart);
        const auto cSkipOne = [](uint32_t aFormId)
        {
            return aFormId == 1 ? Readiness::Skip : Readiness::Go;
        };
        CHECK(pacer.Next(cStart, cSkipOne).FormId == 2);
        CHECK(pacer.Next(cStart, cSkipOne).FormId == 0);
        CHECK(pacer.Next(cStart + ActivationCatchUp::kGap, cSkipOne).FormId == 3);
    }

    SECTION("a load or a disconnection drops them all")
    {
        pacer.Add(1, cStart);
        pacer.Clear();
        CHECK(pacer.Next(cStart, cAlways).FormId == 0);
    }
}

TEST_CASE("what a replayed lever's script uses as this player soon after is not sent", "[activation_catch_up]")
{
    using Clock = std::chrono::steady_clock;
    const auto cNow = Clock::now();
    CHECK_FALSE(ActivationCatchUp::FollowsReplay(Clock::time_point{}, cNow));
    CHECK(ActivationCatchUp::FollowsReplay(cNow, cNow + std::chrono::seconds(1)));
    CHECK_FALSE(ActivationCatchUp::FollowsReplay(cNow, cNow + ActivationCatchUp::kChainWindow));
}

TEST_CASE("a member's switches are taken only within the window after a use of theirs", "[activation_catch_up][sharedhomes]")
{
    ActivationLedger<Place> ledger;
    const auto cNow = ActivationLedger<Place>::Clock::now();

    CHECK_FALSE(ledger.UsedAnyRecently("Brother", cNow));
    ledger.NoteUse("Brother", 0x1234, cNow);
    CHECK(ledger.UsedAnyRecently("Brother", cNow));
    CHECK(ledger.UsedAnyRecently("Brother", cNow + ActivationLedger<Place>::kStateWindow));
    CHECK_FALSE(ledger.UsedAnyRecently("Brother", cNow + ActivationLedger<Place>::kStateWindow + std::chrono::seconds(1)));
    // Another player's use is not this player's.
    CHECK_FALSE(ledger.UsedAnyRecently("Leader", cNow));
    // The server's window and the game's are the same, so a switch the game shares is one the server takes.
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(ActivationLedger<Place>::kStateWindow).count() == static_cast<int64_t>(SharedHomes::kSwitchAfterUseMs));
}
