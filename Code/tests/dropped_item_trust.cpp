#include <catch2/catch.hpp>

#include <chrono>
#include <cstdint>
#include <map>

#include <DroppedItemTrust.h>

using namespace DroppedItemTrust;
using namespace std::chrono_literals;

TEST_CASE("A new drop is added only by a player beside it", "[droppeditemtrust]")
{
    CHECK(MayAdd(false, true, true, true));
    CHECK_FALSE(MayAdd(false, true, true, false));
    CHECK_FALSE(MayAdd(false, false, false, false));
}

TEST_CASE("An announced item needs the loaded grid once the player's cell is known", "[droppeditemtrust]")
{
    CHECK(MayAdd(true, true, true, false));
    CHECK_FALSE(MayAdd(true, true, false, false));
    CHECK_FALSE(MayAdd(true, true, false, true));
}

TEST_CASE("An announcement before the first cell entry is let through", "[droppeditemtrust]")
{
    CHECK(MayAdd(true, false, false, false));
}

TEST_CASE("A removal needs the player near the item", "[droppeditemtrust]")
{
    CHECK(MayRemove(true));
    CHECK_FALSE(MayRemove(false));
}

TEST_CASE("The burst after a load is all let through", "[droppeditemtrust]")
{
    AddBudget budget;
    const auto cNow = AddBudget::Clock::now();
    // A game announces up to a thousand and twenty-four items at once, with a few drops of the session on top.
    for (int i = 0; i < 1100; ++i)
        REQUIRE(budget.Allow(cNow));
}

TEST_CASE("Adds beyond the burst are refused until time passes", "[droppeditemtrust]")
{
    AddBudget budget;
    auto now = AddBudget::Clock::now();
    int cAllowed = 0;
    for (int i = 0; i < 5000; ++i)
        cAllowed += budget.Allow(now) ? 1 : 0;
    CHECK(cAllowed == static_cast<int>(kBurst));
    CHECK_FALSE(budget.Allow(now));

    now += 1s;
    int cAfterOneSecond = 0;
    for (int i = 0; i < 100; ++i)
        cAfterOneSecond += budget.Allow(now) ? 1 : 0;
    CHECK(cAfterOneSecond == static_cast<int>(kPerSecond));
}

TEST_CASE("The budget refills to the burst and no further", "[droppeditemtrust]")
{
    AddBudget budget;
    auto now = AddBudget::Clock::now();
    for (int i = 0; i < 2000; ++i)
        budget.Allow(now);
    now += 1h;
    int cAllowed = 0;
    for (int i = 0; i < 5000; ++i)
        cAllowed += budget.Allow(now) ? 1 : 0;
    CHECK(cAllowed == static_cast<int>(kBurst));
}

TEST_CASE("Honest drops one after another never run out of budget", "[droppeditemtrust]")
{
    AddBudget budget;
    auto now = AddBudget::Clock::now();
    for (int i = 0; i < 20000; ++i)
    {
        now += 2s;
        REQUIRE(budget.Allow(now));
    }
}

TEST_CASE("A full list forgets its oldest items to make room", "[droppeditemtrust]")
{
    std::map<uint32_t, int> items;
    for (uint32_t id = 1; id <= 10; ++id)
        items[id] = static_cast<int>(id);

    CHECK(ForgetOldest(items, 100) == 0);
    CHECK(items.size() == 10);

    CHECK(ForgetOldest(items, 10) == 1);
    CHECK(items.size() == 9);
    CHECK_FALSE(items.contains(1));
    CHECK(items.contains(2));

    CHECK(ForgetOldest(items, 5) == 5);
    CHECK(items.size() == 4);
    CHECK(items.contains(10));
    CHECK_FALSE(items.contains(6));
}

TEST_CASE("Forgetting from an empty list does nothing", "[droppeditemtrust]")
{
    std::map<uint32_t, int> items;
    CHECK(ForgetOldest(items, 0) == 0);
    CHECK(ForgetOldest(items, 3) == 0);
}
