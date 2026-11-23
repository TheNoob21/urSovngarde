#include <catch2/catch.hpp>

#include <ReferenceStateSettle.h>
#include <TrapSync.h>

#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace ReferenceStates;
using namespace std::chrono_literals;

namespace
{
const auto cStart = std::chrono::steady_clock::time_point{} + 1h;
} // namespace

TEST_CASE("a bear trap is used until it is as another game has it, and only this game's own changes are sent", "[reference_state_settle]")
{
    BearTraps traps;
    const auto cLoaded = [](uint32_t)
    {
        return true;
    };
    constexpr uint32_t cId = 0x7A11E;

    SECTION("a load arms it and is not sent; a step or a use here is")
    {
        CHECK_FALSE(traps.Played(cId, true, true, cStart));
        CHECK(traps.IsArmed(cId) == true);
        CHECK(traps.Played(cId, false, false, cStart + 5s));
        CHECK(traps.IsArmed(cId) == false);
    }

    SECTION("an armed trap another game has sprung is used once, and that use's animation is not sent back")
    {
        traps.Played(cId, true, true, cStart);
        traps.Want(cId, false, cStart + 5s);
        CHECK(traps.Next(cStart + 5s, cLoaded) == cId);
        CHECK(traps.Next(cStart + 5s, cLoaded) == 0);
        CHECK_FALSE(traps.Played(cId, false, false, cStart + 5s + 200ms));
        CHECK(traps.Next(cStart + 10s, cLoaded) == 0);
        CHECK(traps.WantedCount() == 0);
    }

    SECTION("a trap already so is left alone; one with no animation since it loaded is sprung, as its script starts")
    {
        traps.Want(cId, false, cStart);
        CHECK(traps.Next(cStart, cLoaded) == 0);
        CHECK(traps.WantedCount() == 0);
    }

    SECTION("a busy trap waits; one that will not change is given up after two uses")
    {
        traps.Played(cId, true, false, cStart);
        traps.Want(cId, false, cStart + 1s);
        CHECK(traps.Next(cStart + 1s, cLoaded) == 0);
        CHECK(traps.Next(cStart + 3s, cLoaded) == cId);
        CHECK(traps.Next(cStart + 6s, cLoaded) == cId);
        CHECK(traps.Next(cStart + 9s, cLoaded) == 0);
        CHECK(traps.WantedCount() == 0);
    }

    SECTION("one not loaded here waits, and is passed over after a while")
    {
        traps.Want(cId, true, cStart);
        CHECK(traps.Next(cStart + 1s, [](uint32_t) { return false; }) == 0);
        CHECK(traps.WantedCount() == 1);
        CHECK(traps.Next(cStart + kWait, [](uint32_t) { return false; }) == 0);
        CHECK(traps.WantedCount() == 0);
    }

    SECTION("this game's own change wins over an older word")
    {
        traps.Want(cId, true, cStart);
        CHECK(traps.Played(cId, false, false, cStart + 1s));
        CHECK(traps.WantedCount() == 0);
    }

    SECTION("the traps remembered are bounded")
    {
        for (uint32_t i = 0; i < BearTraps::kMaxTraps + 5; ++i)
            traps.Played(0x1000 + i, true, true, cStart + std::chrono::seconds(i));
        CHECK(traps.SeenCount() == BearTraps::kMaxTraps);
        CHECK_FALSE(traps.IsArmed(0x1000).has_value());
    }
}

TEST_CASE("a destructible is damaged a little at a time until it reaches another game's stage", "[reference_state_settle]")
{
    StageSettle settle;
    constexpr uint32_t cId = 0x10DCC9;
    // A vein: health 100, its one stage at half.
    float health = 100.f;
    std::vector<float> damages;
    uint32_t reached = 0;
    const auto cStageOf = [&health](uint32_t) -> std::optional<int32_t>
    {
        return health <= 50.f ? 1 : 0;
    };
    const auto cDamage = [&](uint32_t, float aDamage)
    {
        damages.push_back(aDamage);
        health -= aDamage;
    };
    const auto cReached = [&reached](uint32_t aFormId)
    {
        reached = aFormId;
    };

    SECTION("the damage doubles while the stage holds, and the reference is done at the stage")
    {
        settle.Add(cId, 1, cStart);
        auto now = cStart;
        for (int i = 0; i < 20 && reached == 0; ++i, now += StageSettle::kStepGap)
            settle.Update(now, cStageOf, cDamage, cReached);
        CHECK(reached == cId);
        CHECK(damages == std::vector<float>{1.f, 2.f, 4.f, 8.f, 16.f, 32.f});
        CHECK(settle.Size() == 0);
    }

    SECTION("a step waits for the last one to show")
    {
        settle.Add(cId, 1, cStart);
        settle.Update(cStart, cStageOf, cDamage, cReached);
        settle.Update(cStart + 50ms, cStageOf, cDamage, cReached);
        CHECK(damages.size() == 1);
        CHECK(settle.IsSettling(cId));
    }

    SECTION("one already at the stage here is done at once")
    {
        health = 40.f;
        settle.Add(cId, 1, cStart);
        settle.Update(cStart, cStageOf, cDamage, cReached);
        CHECK(damages.empty());
        CHECK(reached == cId);
    }

    SECTION("the damage starts small again at each new stage")
    {
        // An urn: stages at two thirds, one third and nothing.
        float urn = 100.f;
        const auto cUrnStage = [&urn](uint32_t) -> std::optional<int32_t>
        {
            return urn <= 0.f ? 3 : urn <= 33.f ? 2 : urn <= 66.f ? 1 : 0;
        };
        const auto cUrnDamage = [&](uint32_t, float aDamage)
        {
            damages.push_back(aDamage);
            urn -= aDamage;
        };
        settle.Add(cId, 2, cStart);
        auto now = cStart;
        for (int i = 0; i < 30 && reached == 0; ++i, now += StageSettle::kStepGap)
            settle.Update(now, cUrnStage, cUrnDamage, cReached);
        CHECK(reached == cId);
        CHECK(cUrnStage(cId) == 2);
        CHECK(damages == std::vector<float>{1.f, 2.f, 4.f, 8.f, 16.f, 32.f, 1.f, 2.f, 4.f});
    }

    SECTION("one that does not break is left after so many steps")
    {
        const auto cNoStage = [](uint32_t) -> std::optional<int32_t>
        {
            return 0;
        };
        settle.Add(cId, 1, cStart);
        auto now = cStart;
        for (uint32_t i = 0; i < StageSettle::kMaxSteps + 2; ++i, now += StageSettle::kStepGap)
            settle.Update(now, cNoStage, cDamage, cReached);
        CHECK(damages.size() == StageSettle::kMaxSteps);
        CHECK(damages.back() == StageSettle::kMaxStep);
        CHECK(settle.Size() == 0);
        CHECK(reached == 0);
    }

    SECTION("one that is not to be set here is dropped at once")
    {
        const auto cNotShared = [](uint32_t) -> std::optional<int32_t>
        {
            return -1;
        };
        settle.Add(cId, 1, cStart);
        settle.Update(cStart, cNotShared, cDamage, cReached);
        CHECK(settle.Size() == 0);
        CHECK(damages.empty());
        CHECK(reached == 0);
    }

    SECTION("one not loaded waits, and is passed over after a while; a higher stage replaces a lower one waiting")
    {
        const auto cUnloaded = [](uint32_t) -> std::optional<int32_t>
        {
            return std::nullopt;
        };
        settle.Add(cId, 1, cStart);
        settle.Add(cId, 2, cStart);
        settle.Add(cId, 1, cStart);
        CHECK(settle.Size() == 1);
        settle.Update(cStart + 1s, cUnloaded, cDamage, cReached);
        CHECK(settle.Size() == 1);
        settle.Update(cStart + kWait, cUnloaded, cDamage, cReached);
        CHECK(settle.Size() == 0);
        CHECK(damages.empty());
    }
}

TEST_CASE("another game's picked plant waits for its reference to load and settle", "[reference_state_settle]")
{
    Pending pending;
    bool loaded = false;
    const auto cLoaded = [&loaded](uint32_t)
    {
        return loaded;
    };

    pending.Add(0x4B9A1, 1, cStart);
    CHECK(pending.Ready(cStart + 1s, cLoaded).empty());
    loaded = true;
    CHECK(pending.Ready(cStart + 2s, cLoaded).empty());
    const auto cReady = pending.Ready(cStart + 2s + Pending::kSettle, cLoaded);
    REQUIRE(cReady.size() == 1);
    CHECK(cReady[0].first == 0x4B9A1);
    CHECK(pending.Size() == 0);

    SECTION("one not loaded in time is passed over")
    {
        loaded = false;
        pending.Add(0x4B9A2, 1, cStart);
        CHECK(pending.Ready(cStart + kWait, cLoaded).empty());
        CHECK(pending.Size() == 0);
    }

    SECTION("a handful at once, the rest on the next update")
    {
        for (uint32_t i = 0; i < Pending::kPerUpdate + 3; ++i)
            pending.Add(0x5000 + i, 1, cStart);
        pending.Ready(cStart, cLoaded);
        CHECK(pending.Ready(cStart + Pending::kSettle, cLoaded).size() == Pending::kPerUpdate);
        CHECK(pending.Ready(cStart + Pending::kSettle, cLoaded).size() == 3);
    }
}

TEST_CASE("a use here is sent once the game shows it, and a reset once the cell brings it back", "[reference_state_settle]")
{
    UseWatch watch;
    std::unordered_map<uint32_t, Look> looks;
    std::vector<std::pair<uint32_t, bool>> sent;
    const auto cLook = [&looks](uint32_t aFormId)
    {
        const auto cIt = looks.find(aFormId);
        return cIt == looks.end() ? Look::Unloaded : cIt->second;
    };
    const auto cSend = [&sent](uint32_t aFormId, bool aUsed)
    {
        sent.emplace_back(aFormId, aUsed);
    };
    constexpr uint32_t cId = 0x4B9A1;

    SECTION("this player's pick is sent once the plant shows picked, and its regrowth once it shows new")
    {
        looks[cId] = Look::New;
        watch.Used(cId, cStart);
        watch.Update(cStart, cLook, cSend);
        CHECK(sent.empty());
        looks[cId] = Look::Used;
        watch.Update(cStart + 1s, cLook, cSend);
        watch.Update(cStart + 2s, cLook, cSend);
        CHECK(sent == std::vector<std::pair<uint32_t, bool>>{{cId, true}});
        looks[cId] = Look::Unloaded;
        watch.Update(cStart + 10h, cLook, cSend);
        looks[cId] = Look::New;
        watch.Update(cStart + 300h, cLook, cSend);
        CHECK(sent == std::vector<std::pair<uint32_t, bool>>{{cId, true}, {cId, false}});
        CHECK_FALSE(watch.IsWatched(cId));
    }

    SECTION("a use that never shows is forgotten")
    {
        looks[cId] = Look::New;
        watch.Used(cId, cStart);
        watch.Update(cStart + UseWatch::kCheck, cLook, cSend);
        CHECK_FALSE(watch.IsWatched(cId));
        CHECK(sent.empty());
    }

    SECTION("a rune trap is sent as it goes off, and one whose cell unloads first is dropped unsent")
    {
        looks[cId] = Look::New;
        watch.Armed(cId);
        watch.Update(cStart, cLook, cSend);
        CHECK(sent.empty());
        looks[cId] = Look::Used;
        watch.Update(cStart + 2s, cLook, cSend);
        CHECK(sent == std::vector<std::pair<uint32_t, bool>>{{cId, true}});
        looks[0x5000] = Look::Unloaded;
        watch.Armed(0x5000);
        watch.Update(cStart + 4s, cLook, cSend);
        CHECK_FALSE(watch.IsWatched(0x5000));
    }

    SECTION("one used up here for another game is not sent, but its reset is; one where it did not take is forgotten")
    {
        looks[cId] = Look::Used;
        watch.Applied(cId, cStart);
        watch.Update(cStart, cLook, cSend);
        CHECK(sent.empty());
        CHECK(watch.IsWatched(cId));
        looks[cId] = Look::New;
        watch.Update(cStart + 300h, cLook, cSend);
        CHECK(sent == std::vector<std::pair<uint32_t, bool>>{{cId, false}});

        sent.clear();
        looks[0x5001] = Look::New;
        watch.Applied(0x5001, cStart);
        watch.Update(cStart + UseWatch::kCheck, cLook, cSend);
        CHECK(sent.empty());
        CHECK_FALSE(watch.IsWatched(0x5001));
    }
}

TEST_CASE("another game's state is not forced over this game's own reset of the reference just now", "[reference_state_settle]")
{
    Resets resets;
    constexpr uint32_t cId = 0x10DCC9;

    resets.Note(cId, cStart);
    CHECK(resets.IsRecent(cId, cStart + Resets::kWindow - 1s));
    CHECK_FALSE(resets.IsRecent(cId, cStart + Resets::kWindow));
    CHECK_FALSE(resets.IsRecent(0x4B9A1, cStart));
    resets.Forget(cStart + Resets::kWindow);
    CHECK(resets.Size() == 0);

    SECTION("what waits for it is dropped")
    {
        StageSettle settle;
        Pending pending;
        settle.Add(cId, 1, cStart);
        settle.Add(0x5000, 1, cStart);
        pending.Add(cId, 1, cStart);
        settle.Cancel(cId);
        pending.Cancel(cId);
        CHECK_FALSE(settle.IsSettling(cId));
        CHECK(settle.IsSettling(0x5000));
        CHECK(pending.Size() == 0);
    }
}

TEST_CASE("this game's reports go a few a second, a burst waiting its turn", "[reference_state_settle]")
{
    SendPacer<int> pacer;
    std::vector<int> sent;
    const auto cSend = [&sent](int aReport)
    {
        sent.push_back(aReport);
    };

    for (int i = 0; i < 10; ++i)
        CHECK(pacer.Push(i));
    pacer.Drain(cStart, cSend);
    CHECK(sent.size() == SendPacer<int>::kPerSecond);
    pacer.Drain(cStart + 900ms, cSend);
    CHECK(sent.size() == SendPacer<int>::kPerSecond);
    pacer.Drain(cStart + 1s, cSend);
    CHECK(sent == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
    CHECK(pacer.Size() == 0);
    static_assert(SendPacer<int>::kPerSecond < TrapSync::RateLimit::kPerSecond);

    SECTION("past the cap the oldest waiting goes")
    {
        for (size_t i = 0; i < SendPacer<int>::kMaxWaiting; ++i)
            CHECK(pacer.Push(100));
        CHECK_FALSE(pacer.Push(200));
        CHECK(pacer.Size() == SendPacer<int>::kMaxWaiting);
    }
}
