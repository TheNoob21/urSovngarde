#include <catch2/catch.hpp>

#include <Systems/CopyAnimationQueue.h>

#include <thread>

TEST_CASE("Copies' animation steps queued by the actor jobs run once each between frames", "[copy_animation_queue]")
{
    CopyAnimationQueue queue;

    SECTION("each copy once, with the time of every job that queued it")
    {
        queue.Add(0xFF0008EF, 0.011f);
        queue.Add(0xFF0008EF, 0.011f);
        queue.Add(0xFF000900, 0.016f);
        auto steps = queue.Take();
        REQUIRE(steps.size() == 2);
        for (const auto& [cFormId, cStep] : steps)
            CHECK(cStep == Approx(cFormId == 0xFF0008EF ? 0.022f : 0.016f));
        CHECK(queue.Take().empty());
    }

    SECTION("a copy that missed frames is not jumped far ahead")
    {
        for (int i = 0; i < 100; ++i)
            queue.Add(0xFF0008EF, 0.016f);
        const auto cSteps = queue.Take();
        REQUIRE(cSteps.size() == 1);
        CHECK(cSteps[0].second == Approx(CopyAnimationQueue::kMaxStep));
    }

    SECTION("no form, or no time, queues nothing")
    {
        queue.Add(0, 0.016f);
        queue.Add(0xFF0008EF, 0.f);
        queue.Add(0xFF0008EF, -1.f);
        CHECK(queue.Take().empty());
    }

    SECTION("jobs on several threads queue at once")
    {
        std::thread first(
            [&queue]
            {
                for (uint32_t i = 1; i <= 500; ++i)
                    queue.Add(i, 0.001f);
            });
        std::thread second(
            [&queue]
            {
                for (uint32_t i = 1; i <= 500; ++i)
                    queue.Add(i, 0.001f);
            });
        first.join();
        second.join();
        const auto cSteps = queue.Take();
        CHECK(cSteps.size() == 500);
        for (const auto& [cFormId, cStep] : cSteps)
            CHECK(cStep == Approx(0.002f));
    }
}
