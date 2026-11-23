#include <catch2/catch.hpp>

#include <Services/Generic/AlchemyMadeRules.h>

using namespace AlchemyMadeRules;
using namespace std::chrono_literals;

namespace
{
constexpr uint32_t kActor = 0xFF000801;
constexpr uint32_t kBlueMountain = 0x77E1C;
constexpr uint32_t kWheat = 0x4B0BA;
} // namespace

TEST_CASE("Ingredients used up, then a made potion: one potion sound", "[alchemymade]")
{
    Tracker tracker;
    const auto t0 = Clock::time_point{} + 10s;
    REQUIRE(tracker.NoteIngredientUsed(kActor, kBlueMountain, t0) == Made::None);
    REQUIRE(tracker.NoteIngredientUsed(kActor, kWheat, t0 + 10ms) == Made::None);
    REQUIRE(tracker.NoteMade(kActor, false, t0 + 50ms) == Made::Potion);
}

TEST_CASE("A made poison first, then its ingredients: one poison sound", "[alchemymade]")
{
    Tracker tracker;
    const auto t0 = Clock::time_point{} + 10s;
    REQUIRE(tracker.NoteMade(kActor, true, t0) == Made::None);
    REQUIRE(tracker.NoteIngredientUsed(kActor, kBlueMountain, t0 + 20ms) == Made::Poison);
    REQUIRE(tracker.NoteIngredientUsed(kActor, kWheat, t0 + 30ms) == Made::None);
}

TEST_CASE("Two makings in a row play two sounds, never more than the items made", "[alchemymade]")
{
    Tracker tracker;
    const auto t0 = Clock::time_point{} + 10s;
    int sounds = 0;
    for (int making = 0; making < 2; ++making)
    {
        const auto at = t0 + making * 400ms;
        sounds += tracker.NoteIngredientUsed(kActor, kBlueMountain, at) != Made::None;
        sounds += tracker.NoteIngredientUsed(kActor, kWheat, at + 5ms) != Made::None;
        sounds += tracker.NoteMade(kActor, false, at + 10ms) != Made::None;
    }
    REQUIRE(sounds == 2);
}

TEST_CASE("Makings whose made item comes first: the late ingredients add no sound", "[alchemymade]")
{
    Tracker tracker;
    const auto t0 = Clock::time_point{} + 10s;
    int sounds = 0;
    for (int making = 0; making < 3; ++making)
    {
        const auto at = t0 + making * 300ms;
        sounds += tracker.NoteMade(kActor, making == 1, at) != Made::None;
        sounds += tracker.NoteIngredientUsed(kActor, kBlueMountain, at + 5ms) != Made::None;
        sounds += tracker.NoteIngredientUsed(kActor, kWheat, at + 10ms) != Made::None;
    }
    REQUIRE(sounds == 3);
}

TEST_CASE("The making window is kTogether, inclusive", "[alchemymade]")
{
    const auto t0 = Clock::time_point{} + 10s;
    Tracker atEdge;
    atEdge.NoteIngredientUsed(kActor, kWheat, t0);
    REQUIRE(atEdge.NoteMade(kActor, false, t0 + kTogether) == Made::Potion);
    Tracker past;
    past.NoteIngredientUsed(kActor, kWheat, t0);
    REQUIRE(past.NoteMade(kActor, false, t0 + kTogether + 1ms) == Made::None);
}

TEST_CASE("An ingredient put in a chest and a made potion taken out close together count as a making (known limit)", "[alchemymade]")
{
    Tracker tracker;
    const auto t0 = Clock::time_point{} + 10s;
    tracker.NoteIngredientUsed(kActor, kWheat, t0);
    REQUIRE(tracker.NoteMade(kActor, false, t0 + 500ms) == Made::Potion);
}

TEST_CASE("A made potion alone (taken from a chest, bought) or an ingredient alone plays nothing", "[alchemymade]")
{
    Tracker tracker;
    const auto t0 = Clock::time_point{} + 10s;
    REQUIRE(tracker.NoteMade(kActor, false, t0) == Made::None);
    REQUIRE(tracker.NoteIngredientUsed(kActor, kWheat, t0 + kTogether + 1ms) == Made::None);

    Tracker other;
    REQUIRE(other.NoteIngredientUsed(kActor, kWheat, t0) == Made::None);
    REQUIRE(other.NoteMade(kActor, false, t0 + kTogether + 1ms) == Made::None);
}

TEST_CASE("An ingredient eaten is no making, whichever message comes first", "[alchemymade]")
{
    const auto t0 = Clock::time_point{} + 10s;

    Tracker eatFirst;
    eatFirst.NoteEaten(kActor, kWheat, t0);
    REQUIRE(eatFirst.NoteIngredientUsed(kActor, kWheat, t0 + 5ms) == Made::None);
    REQUIRE(eatFirst.NoteMade(kActor, false, t0 + 10ms) == Made::None);

    Tracker removalFirst;
    REQUIRE(removalFirst.NoteIngredientUsed(kActor, kWheat, t0) == Made::None);
    removalFirst.NoteEaten(kActor, kWheat, t0 + 5ms);
    REQUIRE(removalFirst.NoteMade(kActor, false, t0 + 10ms) == Made::None);
}

TEST_CASE("Characters are tracked apart, and the table stays small", "[alchemymade]")
{
    Tracker tracker;
    const auto t0 = Clock::time_point{} + 10s;
    REQUIRE(tracker.NoteIngredientUsed(kActor, kWheat, t0) == Made::None);
    REQUIRE(tracker.NoteMade(kActor + 1, false, t0) == Made::None);
    for (uint32_t i = 0; i < 200; ++i)
        tracker.NoteMade(0x100 + i, false, t0);
    REQUIRE(tracker.Size() <= kMaxTracked);
}

TEST_CASE("A making far from the player here is not heard", "[alchemymade]")
{
    REQUIRE(Heard(200.f));
    REQUIRE(Heard(kHearWithin));
    REQUIRE_FALSE(Heard(kHearWithin + 1.f));
}
