#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <glm/vec3.hpp>

#include <catch2/catch.hpp>

#include <ObjectMover.h>
#include <DeferredSave.h>
#include <PropBodyRelay.h>
#include <Services/QuestAliasDeciders.h>

#include <chrono>
#include <map>
#include <vector>

TEST_CASE("Idle object movers are swept once a map of them has grown, and moving ones stay", "[object_places]")
{
    using Clock = ObjectMover::Clock;
    const auto cStart = Clock::now();
    std::map<uint64_t, ObjectMover> movers;
    for (uint64_t key = 1; key <= 10; ++key)
        movers[key].Take(1, false, cStart);
    movers[11].Take(2, false, cStart + std::chrono::seconds(5));

    ForgetLapsedMovers(movers, cStart + std::chrono::seconds(5), 20);
    CHECK(movers.size() == 11);

    ForgetLapsedMovers(movers, cStart + std::chrono::seconds(5), 4);
    REQUIRE(movers.size() == 1);
    CHECK(movers.begin()->first == 11);
}

TEST_CASE("A full map of object places drops its oldest to make room, never the one being written", "[object_places]")
{
    std::map<uint64_t, int> places{{1, 30}, {2, 10}, {3, 20}};
    const auto cAge = [](int aSeen)
    {
        return aSeen;
    };

    MakeRoom(places, 4, 9, cAge);
    CHECK(places.size() == 3);

    MakeRoom(places, 3, 9, cAge);
    CHECK(places.size() == 2);
    CHECK_FALSE(places.contains(2));

    MakeRoom(places, 2, 1, cAge);
    CHECK(places.size() == 2);
}

TEST_CASE("A player's object moves are capped each second, as hinged bodies are", "[object_places]")
{
    using Clock = PropBodyRelay::Clock;
    const auto cStart = Clock::now();
    PropBodyRelay::Rate rate;
    uint32_t allowed = 0;
    for (int i = 0; i < 300; ++i)
        allowed += rate.Allow(cStart + std::chrono::milliseconds(i)) ? 1 : 0;
    CHECK(allowed == PropBodyRelay::kUpdatesPerSecond);
    CHECK(rate.Allow(cStart + std::chrono::seconds(1)));

    PropBodyRelay::Rate small;
    CHECK(small.Allow(cStart, 2));
    CHECK(small.Allow(cStart, 2));
    CHECK_FALSE(small.Allow(cStart, 2));
}

TEST_CASE("A disbanded party's quest deciders are forgotten", "[object_places]")
{
    QuestAliasDeciders deciders;
    const GameId cQuest(0, 0x3372B);
    deciders.OnStarted(7, cQuest, 1);
    deciders.OnStarted(8, cQuest, 2);
    REQUIRE(deciders.DeciderOf(7, cQuest) == 1u);

    deciders.ForgetParty(7);
    CHECK_FALSE(deciders.DeciderOf(7, cQuest).has_value());
    CHECK(deciders.DeciderOf(8, cQuest) == 2u);
}

TEST_CASE("The server's file is written at most once every five seconds, and no change waits longer", "[object_places]")
{
    using Clock = DeferredSave::Clock;
    const auto cStart = Clock::time_point{} + std::chrono::hours(1);

    SECTION("a burst of rests is one write now and one five seconds later")
    {
        DeferredSave save;
        CHECK_FALSE(save.Due(cStart));
        save.Mark();
        CHECK(save.Due(cStart));
        CHECK_FALSE(save.IsDirty());

        uint32_t writes = 0;
        for (int i = 1; i <= 30; ++i)
        {
            save.Mark();
            writes += save.Due(cStart + std::chrono::milliseconds(33 * i)) ? 1 : 0;
        }
        CHECK(writes == 0);
        CHECK(save.IsDirty());
        CHECK_FALSE(save.Due(cStart + std::chrono::milliseconds(4999)));
        CHECK(save.Due(cStart + DeferredSave::kGap));
        CHECK_FALSE(save.Due(cStart + std::chrono::seconds(20)));
    }

    SECTION("the last player leaving or the server stopping writes at once, and only what changed")
    {
        DeferredSave save;
        CHECK_FALSE(save.Flush(cStart));
        save.Mark();
        CHECK(save.Due(cStart));
        save.Mark();
        CHECK_FALSE(save.Due(cStart + std::chrono::seconds(1)));
        CHECK(save.Flush(cStart + std::chrono::seconds(1)));
        CHECK_FALSE(save.Flush(cStart + std::chrono::seconds(1)));
        save.Mark();
        CHECK_FALSE(save.Due(cStart + std::chrono::seconds(2)));
        CHECK(save.Due(cStart + std::chrono::seconds(6)));
    }

    SECTION("changes at any moment on a 60 Hz server tick")
    {
        constexpr auto cTick = std::chrono::milliseconds(16);
        const std::vector<int> cChangesMs{0, 5, 40, 900, 4990, 5010, 5100, 12000, 12001, 30500, 30516, 34000};
        DeferredSave save;
        std::vector<Clock::time_point> written;
        std::vector<Clock::time_point> pending;
        std::chrono::milliseconds longestWait{};
        size_t next = 0;
        for (auto now = cStart; now < cStart + std::chrono::seconds(45); now += cTick)
        {
            for (; next < cChangesMs.size() && cStart + std::chrono::milliseconds(cChangesMs[next]) <= now; ++next)
            {
                save.Mark();
                pending.push_back(cStart + std::chrono::milliseconds(cChangesMs[next]));
            }
            if (!save.Due(now))
                continue;
            written.push_back(now);
            for (const auto cChanged : pending)
                longestWait = std::max(longestWait, std::chrono::duration_cast<std::chrono::milliseconds>(now - cChanged));
            pending.clear();
        }
        CHECK(pending.empty());
        CHECK_FALSE(save.IsDirty());
        CHECK(longestWait <= DeferredSave::kGap + cTick);
        for (size_t i = 1; i < written.size(); ++i)
            CHECK(written[i] - written[i - 1] >= DeferredSave::kGap);
    }
}

TEST_CASE("Only moves the mover takes count against a player's moves per second", "[object_places]")
{
    using Clock = ObjectMover::Clock;
    const auto cStart = Clock::time_point{} + std::chrono::hours(1);
    std::map<uint32_t, ObjectMover> movers;
    PropBodyRelay::Rate first;
    PropBodyRelay::Rate second;

    SECTION("asking changes nothing")
    {
        ObjectMover mover;
        REQUIRE(mover.Take(1, false, cStart, 100));
        for (int i = 0; i < 3; ++i)
        {
            CHECK_FALSE(mover.Accepts(2, cStart + std::chrono::milliseconds(10)));
            CHECK_FALSE(mover.Accepts(1, cStart + std::chrono::milliseconds(10), 99));
            CHECK(mover.Accepts(1, cStart + std::chrono::milliseconds(10), 101));
        }
        CHECK(mover.GetPlayerId() == 1u);
        CHECK_FALSE(mover.IsLapsed(cStart + ObjectMover::kHeldFor - std::chrono::milliseconds(1)));
        CHECK_FALSE(mover.Take(1, false, cStart + std::chrono::milliseconds(20), 99));
        CHECK(mover.Take(1, false, cStart + std::chrono::milliseconds(20), 100));
    }

    SECTION("pushing an object another player carries leaves the pusher's own carry its full rate")
    {
        REQUIRE(TakeCapped(movers, 1u, first, 1, false, cStart, 1));
        uint32_t refused = 0;
        for (int i = 0; i < 300; ++i)
            refused += TakeCapped(movers, 1u, second, 2, false, cStart + std::chrono::milliseconds(i), 0) ? 0 : 1;
        CHECK(refused == 300);
        CHECK(movers[1].GetPlayerId() == 1u);

        uint32_t taken = 0;
        for (int i = 0; i < 300; ++i)
            taken += TakeCapped(movers, 2u, second, 2, false, cStart + std::chrono::milliseconds(300 + i), 0) ? 1 : 0;
        CHECK(taken == PropBodyRelay::kUpdatesPerSecond);
    }

    SECTION("late moves of the mover's own spend nothing, and a put-down always goes")
    {
        REQUIRE(TakeCapped(movers, 1u, first, 1, false, cStart, 500));
        for (int i = 0; i < 200; ++i)
            CHECK_FALSE(TakeCapped(movers, 1u, first, 1, false, cStart + std::chrono::milliseconds(1), 400));
        uint32_t taken = 1;
        for (uint64_t tick = 501; tick < 700; ++tick)
            taken += TakeCapped(movers, 1u, first, 1, false, cStart + std::chrono::milliseconds(2), tick) ? 1 : 0;
        CHECK(taken == PropBodyRelay::kUpdatesPerSecond);
        CHECK(TakeCapped(movers, 1u, first, 1, true, cStart + std::chrono::milliseconds(3), 700));
        CHECK(movers[1].GetPlayerId() == 1u);
    }
}
