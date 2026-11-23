#include <catch2/catch.hpp>

#include <SaveScope.h>
#include <Services/PlayerMapRules.h>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace
{
using namespace std::chrono_literals;

struct Cell
{
};

struct Position
{
    float X, Y, Z;
};

constexpr size_t kMarkers = 4;
constexpr uint8_t kShownOnly = PlayerMapRules::kMarkerVisible;

// The save hook's side of the markers, as PlayerMarkerService's SaveHoldScope drives the hold, and a mover's side, as MoveMarker.
struct Markers
{
    Cell world;
    Cell home;
    std::array<Cell*, kMarkers> Cells{};
    std::array<Position, kMarkers> Where{};
    std::array<uint8_t, kMarkers> Flags{};
    PlayerMapRules::SaveHold<Cell, Position, kMarkers> Hold;
    std::atomic<int> Restored{0};
    std::chrono::milliseconds MoverWait{50ms};

    Markers()
    {
        std::array<PlayerMapRules::SavedMarker<Cell, Position>, kMarkers> markers{};
        for (size_t i = 0; i < kMarkers; ++i)
        {
            Cells[i] = &world;
            Where[i] = {1000.f + static_cast<float>(i), 0.f, 0.f};
            Flags[i] = kShownOnly;
            markers[i] = {&Cells[i], &Where[i], &Flags[i], Position{static_cast<float>(i) * 64.f, 0.f, 0.f}};
        }
        Hold.Publish(markers, &home);
    }

    void EnterSave() noexcept { Hold.Enter(MoverWait); }

    void LeaveSave() noexcept
    {
        if (Hold.Leave())
            ++Restored;
    }

    bool Move(size_t aIndex, float aX) noexcept
    {
        if (!Hold.BeginMove(aIndex))
            return false;
        Where[aIndex].X = aX;
        Hold.EndMove();
        return true;
    }

    bool Home(size_t aIndex) const noexcept { return Cells[aIndex] == &home && Where[aIndex].X == static_cast<float>(aIndex) * 64.f; }

    bool AllHome() const noexcept
    {
        for (size_t i = 0; i < kMarkers; ++i)
            if (!Home(i))
                return false;
        return true;
    }

    bool AllBack() const noexcept
    {
        for (size_t i = 0; i < kMarkers; ++i)
            if (Cells[i] != &world || Where[i].X != 1000.f + static_cast<float>(i) || Flags[i] != kShownOnly)
                return false;
        return true;
    }
};

// A mask one character's write lifts, as the hidden flag and the seat, dragon and horse guards do.
struct Mask
{
    std::atomic<int> Value{1};
    std::atomic<int> Lifts{0};
};

struct Lift
{
    explicit Lift(Mask& aMask) noexcept
        : mask(aMask)
    {
        ++mask.Lifts;
        mask.Value = 0;
    }
    ~Lift() { mask.Value = 1; }

    Mask& mask;
};

void WriteCharacter(SaveScope::FormsWriting<Lift>& aWriting, Mask& aMask, uint32_t aFormId, const std::function<void()>& acInside)
{
    aWriting.Write(aFormId, [&aMask] { return std::make_unique<Lift>(aMask); }, acInside);
}

// One thread lets another go on at a point it names.
struct Gate
{
    std::promise<void> Opened;
    std::shared_future<void> open{Opened.get_future().share()};

    void Open() { Opened.set_value(); }
    void Wait() const { open.wait(); }
    bool OpenWithin(std::chrono::milliseconds aWithin) const { return open.wait_for(aWithin) == std::future_status::ready; }
};
} // namespace

TEST_CASE("A save holds the markers at home and puts them back once", "[save_scope]")
{
    Markers markers;
    {
        const SaveScope::Scoped cSave(&markers);
        CHECK(markers.AllHome());
        CHECK(markers.Hold.InSave());
        CHECK(markers.Hold.GetCount() == kMarkers);
    }
    CHECK(markers.AllBack());
    CHECK_FALSE(markers.Hold.InSave());
    CHECK(markers.Restored == 1);
}

TEST_CASE("A save that throws still puts the markers back", "[save_scope]")
{
    Markers markers;
    CHECK_THROWS_AS(
        [&]()
        {
            const SaveScope::Scoped cSave(&markers);
            REQUIRE(markers.AllHome());
            throw std::runtime_error("save failed");
        }(),
        std::runtime_error);
    CHECK(markers.AllBack());
    CHECK_FALSE(markers.Hold.InSave());
    CHECK(markers.Restored == 1);
    // And the next save works as the first.
    {
        const SaveScope::Scoped cSave(&markers);
        CHECK(markers.AllHome());
    }
    CHECK(markers.AllBack());
    CHECK(markers.Restored == 2);
}

TEST_CASE("A save nested in another keeps the markers at home until the outer one ends", "[save_scope]")
{
    Markers markers;
    {
        const SaveScope::Scoped cOuter(&markers);
        {
            const SaveScope::Scoped cInner(&markers);
            CHECK(markers.AllHome());
        }
        // The old straight-line hook put them back here, while the outer save was still writing.
        CHECK(markers.AllHome());
        CHECK(markers.Restored == 0);
    }
    CHECK(markers.AllBack());
    CHECK(markers.Restored == 1);
}

TEST_CASE("A nested save that throws leaves the outer save's home in place", "[save_scope]")
{
    Markers markers;
    CHECK_THROWS(
        [&]()
        {
            const SaveScope::Scoped cOuter(&markers);
            try
            {
                const SaveScope::Scoped cInner(&markers);
                throw std::runtime_error("inner save failed");
            }
            catch (const std::runtime_error&)
            {
                CHECK(markers.AllHome());
            }
            throw std::runtime_error("outer save failed");
        }());
    CHECK(markers.AllBack());
    CHECK(markers.Restored == 1);
}

TEST_CASE("Two saves that overlap without nesting: the last to end puts the markers back", "[save_scope]")
{
    Markers markers;
    markers.EnterSave();
    markers.EnterSave();
    markers.LeaveSave(); // the first ends while the second still writes
    CHECK(markers.AllHome());
    markers.LeaveSave();
    CHECK(markers.AllBack());
    CHECK(markers.Restored == 1);
    // An end with no save under way changes nothing.
    markers.LeaveSave();
    CHECK(markers.AllBack());
    CHECK_FALSE(markers.Hold.InSave());
}

TEST_CASE("No marker moves while a save holds them, and one moving when it starts is homed once it lands", "[save_scope]")
{
    Markers markers;
    {
        const SaveScope::Scoped cSave(&markers);
        CHECK_FALSE(markers.Move(1, 5.f));
        CHECK(markers.Where[1].X == 64.f);
    }
    CHECK(markers.AllBack());
    CHECK(markers.Move(1, 5.f));
    CHECK(markers.Where[1].X == 5.f);

    // Marker 2 is being moved (claimed, not yet landed) when the save starts: hidden from the map, not homed.
    Markers moving;
    moving.MoverWait = 1ms;
    REQUIRE(moving.Hold.BeginMove(2));
    moving.EnterSave();
    CHECK(moving.Cells[2] == &moving.world);
    CHECK(moving.Flags[2] == 0);
    CHECK(moving.Home(0));
    CHECK(moving.Hold.GetCount() == kMarkers - 1);
    // It lands during the save: written as at home from then on.
    moving.Where[2].X = 7.f;
    moving.Hold.EndMove();
    CHECK(moving.Home(2));
    moving.LeaveSave();
    CHECK(moving.Cells[2] == &moving.world);
    CHECK(moving.Where[2].X == 7.f);
    CHECK(moving.Flags[2] == kShownOnly);
    CHECK(moving.Cells[0] == &moving.world);
    CHECK(moving.Where[0].X == 1000.f);
}

TEST_CASE("A save never writes our markers as fast travel destinations", "[save_scope]")
{
    Markers markers;
    markers.Flags[1] = kShownOnly | PlayerMapRules::kMarkerCanTravel;
    {
        const SaveScope::Scoped cSave(&markers);
        CHECK(markers.Flags[1] == kShownOnly);
    }
    // Taken back for good, not put back after the save.
    CHECK(markers.Flags[1] == kShownOnly);
    CHECK(markers.AllBack());
}

TEST_CASE("A save whose marker move is stuck behind it stops waiting, keeps that marker off the map, and ends without it", "[save_scope]")
{
    Markers markers;
    markers.MoverWait = 30ms;
    // The mover holds its own lock through the game's move, which waits for the save, as PlayerMarkerService's update does.
    std::mutex serviceLock;
    Gate moving;
    Gate saveDone;
    std::thread mover(
        [&]
        {
            const std::scoped_lock lock(serviceLock);
            if (!markers.Hold.BeginMove(2))
                return;
            moving.Open();
            saveDone.Wait();
            markers.Where[2].X = 9.f;
            markers.Hold.EndMove();
        });
    moving.Wait();

    // The whole save, entry to exit, on its own thread: it must end while the mover is still stuck.
    std::atomic<bool> hidden{false};
    std::atomic<bool> othersHome{false};
    Gate ended;
    std::thread save(
        [&]
        {
            {
                const SaveScope::Scoped cSave(&markers);
                hidden = markers.Flags[2] == 0 && markers.Cells[2] == &markers.world;
                othersHome = markers.Home(0) && markers.Home(1) && markers.Home(3);
            }
            ended.Open();
        });
    const bool cEnded = ended.OpenWithin(2s);
    CHECK(cEnded);
    CHECK(hidden);
    CHECK(othersHome);
    CHECK(markers.Restored == 1);
    CHECK_FALSE(markers.Hold.InSave());
    // Everything is back, the stuck marker's map flags included, while its move has still not landed.
    CHECK(markers.Flags[2] == kShownOnly);
    CHECK(markers.Cells[0] == &markers.world);
    CHECK(markers.Where[0].X == 1000.f);

    saveDone.Open();
    save.join();
    mover.join();
    // It lands after the save, where it was sent.
    CHECK(markers.Where[2].X == 9.f);
    CHECK(markers.Cells[2] == &markers.world);
}

TEST_CASE("A save that starts on another thread while the first is still homing the markers waits for that home", "[save_scope]")
{
    Markers markers;
    markers.MoverWait = 2s;
    std::atomic<bool> landed{false};
    REQUIRE(markers.Hold.BeginMove(3));

    Gate firstIn;
    Gate firstMayLeave;
    std::thread first(
        [&]
        {
            markers.EnterSave(); // waits for marker 3's move
            firstIn.Open();
            firstMayLeave.Wait();
            markers.LeaveSave();
        });
    // The first save has started (counted) and waits for the move.
    while (!markers.Hold.InSave())
        std::this_thread::yield();

    std::atomic<bool> landedBeforeSecond{false};
    std::atomic<bool> homeForSecond{false};
    Gate secondIn;
    Gate secondMayLeave;
    std::thread second(
        [&]
        {
            markers.EnterSave();
            landedBeforeSecond = landed.load();
            homeForSecond = markers.AllHome();
            secondIn.Open();
            secondMayLeave.Wait();
            markers.LeaveSave();
        });

    // The second save is held at its entry until the move lands and the first save homes everything.
    CHECK_FALSE(secondIn.OpenWithin(50ms));
    markers.Where[3].X = 1003.f;
    landed = true;
    markers.Hold.EndMove();
    firstIn.Wait();
    secondIn.Wait();
    CHECK(landedBeforeSecond);
    CHECK(homeForSecond);

    // The first ends while the second still writes: they stay home.
    firstMayLeave.Open();
    first.join();
    CHECK(markers.AllHome());
    CHECK(markers.Restored == 0);
    secondMayLeave.Open();
    second.join();
    CHECK(markers.AllBack());
    CHECK(markers.Restored == 1);
}

TEST_CASE("A save that starts while the first is still held by a stuck move is not written before the first's home", "[save_scope]")
{
    Markers markers;
    // Longer than the second save ever waited before: it went ahead at a second, with every marker where it stood.
    markers.MoverWait = 1200ms;
    REQUIRE(markers.Hold.BeginMove(1));

    Gate firstIn;
    Gate firstMayLeave;
    std::thread first(
        [&]
        {
            markers.EnterSave();
            firstIn.Open();
            firstMayLeave.Wait();
            markers.LeaveSave();
        });
    while (!markers.Hold.InSave())
        std::this_thread::yield();

    std::atomic<bool> firstReadyBeforeSecond{false};
    std::atomic<bool> writtenAsHome{false};
    std::thread second(
        [&]
        {
            markers.EnterSave();
            firstReadyBeforeSecond = firstIn.OpenWithin(0ms) || markers.Hold.GetCount() == kMarkers - 1;
            writtenAsHome = markers.Home(0) && markers.Home(2) && markers.Home(3) && markers.Flags[1] == 0;
            markers.LeaveSave();
        });
    second.join();
    CHECK(firstReadyBeforeSecond);
    CHECK(writtenAsHome);
    firstMayLeave.Open();
    first.join();
    markers.Where[1].X = 1001.f;
    markers.Hold.EndMove();
    CHECK(markers.AllBack());
    CHECK(markers.Restored == 1);
}

TEST_CASE("A character written again inside its own write keeps the outer write's mask", "[save_scope]")
{
    SaveScope::FormsWriting<Lift> writing;
    Mask horse;
    WriteCharacter(
        writing, horse, 0x1234,
        [&]()
        {
            CHECK(horse.Value == 0);
            // A nested save writes the same horse.
            WriteCharacter(writing, horse, 0x1234, [&]() { CHECK(horse.Value == 0); });
            // Still masked for the rest of the outer write.
            CHECK(horse.Value == 0);
            CHECK(writing.Writing(0x1234));
        });
    CHECK(horse.Value == 1);
    CHECK(horse.Lifts == 1);
    CHECK_FALSE(writing.Writing(0x1234));
}

TEST_CASE("A character write that throws puts its mask back and is no longer counted as writing", "[save_scope]")
{
    SaveScope::FormsWriting<Lift> writing;
    Mask seat;
    CHECK_THROWS_AS(
        WriteCharacter(writing, seat, 0x42, [&]() { WriteCharacter(writing, seat, 0x42, []() { throw std::runtime_error("nested write failed"); }); }), std::runtime_error);
    CHECK(seat.Value == 1);
    CHECK_FALSE(writing.Writing(0x42));
    // Other characters are their own first writes.
    Mask other;
    WriteCharacter(writing, seat, 0x42, [&]() { WriteCharacter(writing, other, 0x43, [&]() { CHECK(other.Value == 0); }); });
    CHECK(seat.Lifts == 2);
    CHECK(other.Lifts == 1);
    CHECK(other.Value == 1);
}

TEST_CASE("A character written on two threads at once is written masked by both, and unmasked only after the last", "[save_scope]")
{
    SaveScope::FormsWriting<Lift> writing;
    Mask horse;
    Gate firstMasking;
    Gate firstMayMask;
    Gate secondWriting;
    Gate secondMayEnd;
    std::atomic<int> secondSaw{-1};

    std::thread first(
        [&]
        {
            writing.Write(
                0x77,
                [&]
                {
                    firstMasking.Open();
                    firstMayMask.Wait();
                    return std::make_unique<Lift>(horse);
                },
                [] {});
        });
    firstMasking.Wait();

    // The second write comes while the first is still putting its mask on: it is not made before the mask is on.
    std::thread second(
        [&]
        {
            WriteCharacter(
                writing, horse, 0x77,
                [&]
                {
                    secondSaw = horse.Value.load();
                    secondWriting.Open();
                    secondMayEnd.Wait();
                });
        });
    CHECK_FALSE(secondWriting.OpenWithin(50ms));
    CHECK(secondSaw == -1);
    firstMayMask.Open();
    secondWriting.Wait();
    CHECK(secondSaw == 0);

    // The first write ends while the second still writes: the mask stays on.
    first.join();
    CHECK(horse.Value == 0);
    CHECK(writing.Writing(0x77));
    secondMayEnd.Open();
    second.join();
    CHECK(horse.Value == 1);
    CHECK(horse.Lifts == 1);
    CHECK_FALSE(writing.Writing(0x77));
}

namespace
{
// A hidden actor's flag as CharacterSaveMask lifts it: taken off only if it is on, and put back only by the mask that took it off.
struct FlagLift
{
    FlagLift(std::atomic<bool>& aDisabled, Gate* apRestoring, const Gate* apMayRestore) noexcept
        : Disabled(aDisabled)
        , pRestoring(apRestoring)
        , pMayRestore(apMayRestore)
        , Took(aDisabled.exchange(false))
    {
    }
    ~FlagLift()
    {
        if (pRestoring)
            pRestoring->Open();
        if (pMayRestore)
            pMayRestore->Wait();
        if (Took)
            Disabled = true;
    }

    std::atomic<bool>& Disabled;
    Gate* pRestoring;
    const Gate* pMayRestore;
    bool Took;
};
} // namespace

TEST_CASE("A character written while its last write's mask is still coming off waits, then is written under its own mask", "[save_scope]")
{
    SaveScope::FormsWriting<FlagLift> writing;
    std::atomic<bool> disabled{true};
    Gate restoring;
    Gate mayRestore;
    std::thread first([&] { writing.Write(0x88, [&] { return std::make_unique<FlagLift>(disabled, &restoring, &mayRestore); }, [] {}); });
    // The first write is over and its mask is putting the flag back.
    restoring.Wait();

    std::atomic<bool> sawDisabled{true};
    std::atomic<bool> tookItsOwn{false};
    Gate secondInside;
    Gate secondMayRead;
    std::thread second(
        [&]
        {
            writing.Write(
                0x88,
                [&]
                {
                    auto pLift = std::make_unique<FlagLift>(disabled, nullptr, nullptr);
                    tookItsOwn = pLift->Took;
                    return pLift;
                },
                [&]
                {
                    secondInside.Open();
                    secondMayRead.Wait();
                    sawDisabled = disabled.load();
                });
        });
    CHECK_FALSE(secondInside.OpenWithin(50ms));
    CHECK(writing.Writing(0x88));
    mayRestore.Open();
    first.join();
    secondInside.Wait();
    // Put back before the second write began: that write lifted it again, and it stays off for the whole write.
    secondMayRead.Open();
    second.join();
    CHECK(tookItsOwn);
    CHECK_FALSE(sawDisabled);
    CHECK(disabled);
    CHECK_FALSE(writing.Writing(0x88));
}

TEST_CASE("A marker the game made a fast travel destination is shown only again", "[save_scope][player_map]")
{
    using PlayerMapRules::kMarkerCanTravel;
    using PlayerMapRules::kMarkerVisible;
    using PlayerMapRules::TakeTravelBack;

    uint8_t flags = kMarkerVisible | kMarkerCanTravel;
    CHECK(TakeTravelBack(flags));
    CHECK(flags == kMarkerVisible);
    CHECK_FALSE(TakeTravelBack(flags));
    CHECK(flags == kMarkerVisible);

    // Other bits are kept.
    flags = kMarkerCanTravel | 4;
    CHECK(TakeTravelBack(flags));
    CHECK(flags == 4);
}
