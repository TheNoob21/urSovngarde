#include <catch2/catch.hpp>

#include <PvpBlock.h>

using namespace PvpBlock;

TEST_CASE("a hit is seen here as late as its attacker's copy is shown", "[pvp_block]")
{
    REQUIRE(SeenAfter(10000, 0) == kDefaultSeenAfter);
    // A playback tick from the future, or now, is no measure.
    REQUIRE(SeenAfter(10000, 10000) == kDefaultSeenAfter);
    REQUIRE(SeenAfter(10000, 12000) == kDefaultSeenAfter);
    REQUIRE(SeenAfter(10000, 9800) == 200);
    REQUIRE(SeenAfter(10000, 9999) == kMinSeenAfter);
    REQUIRE(SeenAfter(10000, 1000) == kMaxSeenAfter);
}

TEST_CASE("a held block is kept as spans", "[pvp_block]")
{
    History history;
    REQUIRE_FALSE(history.HeldDuring(0, 100000));

    for (uint64_t tick = 1000; tick <= 1200; tick += 16)
        history.Note(tick, true);
    history.Note(1216, false);
    history.Note(1232, false);
    REQUIRE(history.Size() == 1);
    REQUIRE(history.HeldDuring(1100, 1100));
    REQUIRE(history.HeldDuring(900, 1000));
    REQUIRE(history.HeldDuring(1192, 1300));
    REQUIRE_FALSE(history.HeldDuring(1201, 1300));
    REQUIRE_FALSE(history.HeldDuring(0, 999));

    // Raised again: a second span.
    history.Note(1500, true);
    history.Note(1516, true);
    REQUIRE(history.Size() == 2);
    REQUIRE_FALSE(history.HeldDuring(1300, 1499));
    REQUIRE(history.HeldDuring(1300, 1500));

    // Old spans go.
    history.Note(1516 + kKeep + 1, false);
    REQUIRE(history.Size() == 0);

    history.Note(20000, true);
    history.Clear();
    REQUIRE(history.Size() == 0);
    // After a clear, the next block starts a new span rather than stretching a forgotten one.
    history.Note(20100, true);
    REQUIRE_FALSE(history.HeldDuring(20000, 20099));
}

TEST_CASE("a hit is blocked when the block was up as it was seen", "[pvp_block]")
{
    constexpr uint64_t cSeen = 10000;

    SECTION("held through the hit: blocked as soon as it is seen in the window")
    {
        History history;
        for (uint64_t tick = 9500; tick <= 9950; tick += 16)
            history.Note(tick, true);
        REQUIRE(Decide(history, cSeen, 9800) == Verdict::kWait);
        REQUIRE(Decide(history, cSeen, 9950) == Verdict::kBlocked);
        REQUIRE(Decide(history, cSeen, cSeen) == Verdict::kBlocked);
    }

    SECTION("lowered too long before: lands, once the window has passed")
    {
        History history;
        for (uint64_t tick = 9000; tick <= 9890; tick += 10)
            history.Note(tick, true);
        history.Note(9895, false);
        REQUIRE(Decide(history, cSeen, cSeen) == Verdict::kWait);
        REQUIRE(Decide(history, cSeen, cSeen + kBlockAfter - 1) == Verdict::kWait);
        REQUIRE(Decide(history, cSeen, cSeen + kBlockAfter) == Verdict::kLands);
    }

    SECTION("lowered just inside the window: blocked")
    {
        History history;
        history.Note(9800, true);
        history.Note(cSeen - kBlockBefore, true);
        history.Note(cSeen - kBlockBefore + 10, false);
        REQUIRE(Decide(history, cSeen, cSeen + kBlockAfter) == Verdict::kBlocked);
    }

    SECTION("raised just after it was seen: blocked; raised later: lands")
    {
        History early;
        early.Note(cSeen + kBlockAfter, true);
        REQUIRE(Decide(early, cSeen, cSeen + kBlockAfter) == Verdict::kBlocked);

        History late;
        late.Note(cSeen + kBlockAfter + 1, true);
        REQUIRE(Decide(late, cSeen, cSeen + kBlockAfter + 1) == Verdict::kLands);
    }

    SECTION("never held: lands")
    {
        History history;
        REQUIRE(Decide(history, cSeen, cSeen + 1000) == Verdict::kLands);
    }

    SECTION("a seen tick near zero does not wrap")
    {
        History history;
        history.Note(0, true);
        REQUIRE(Decide(history, 20, 20) == Verdict::kBlocked);
    }
}

TEST_CASE("a block faces the one who struck", "[pvp_block]")
{
    constexpr float cPi = 3.14159265358979f;
    // Heading 0 faces +y.
    REQUIRE(Faces(0.f, 0.f, 0.f, 0.f, 100.f));
    REQUIRE_FALSE(Faces(0.f, 0.f, 0.f, 0.f, -100.f));
    // Heading pi/2 faces +x.
    REQUIRE(Faces(0.f, 0.f, cPi / 2.f, 100.f, 0.f));
    REQUIRE_FALSE(Faces(0.f, 0.f, cPi / 2.f, -100.f, 0.f));
    // 45 degrees off the facing is in front, 80 is not.
    REQUIRE(Faces(0.f, 0.f, 0.f, std::sin(45.f * cPi / 180.f) * 100.f, std::cos(45.f * cPi / 180.f) * 100.f));
    REQUIRE_FALSE(Faces(0.f, 0.f, 0.f, std::sin(80.f * cPi / 180.f) * 100.f, std::cos(80.f * cPi / 180.f) * 100.f));
    // Away from the origin, by position.
    REQUIRE(Faces(1000.f, -500.f, 0.f, 1010.f, -400.f));
    // Standing on the same spot counts as faced.
    REQUIRE(Faces(5.f, 5.f, 1.f, 5.5f, 5.f));
}

TEST_CASE("a flat block's clash stops its own blow and no later one", "[pvp_block]")
{
    // The defender's rule matches a clash c to a blow seen at s when c + kRuleBlockBefore >= s and c <= s + 100.
    const auto cMatches = [](const uint64_t aClash, const uint64_t aSeen)
    {
        return aClash + kRuleBlockBefore >= aSeen && aClash <= aSeen + 100;
    };
    constexpr uint64_t cSeen = 20000;
    const uint64_t cClash = ClashTick(cSeen);
    REQUIRE(cMatches(cClash, cSeen));
    // The rule's own reckoning of when it was seen may be a little later, or earlier.
    REQUIRE(cMatches(cClash, cSeen + kSeenMargin));
    REQUIRE(cMatches(cClash, cSeen - 300));
    // A blow seen later than that is weighed on its own.
    REQUIRE_FALSE(cMatches(cClash, cSeen + kSeenMargin + 1));
    REQUIRE_FALSE(cMatches(cClash, cSeen + 500));
    REQUIRE(ClashTick(10) == 0);
}

TEST_CASE("the block's spans edge cases", "[pvp_block]")
{
    History history;
    history.Note(1000, true);
    history.Note(1100, false);
    // Kept exactly kKeep after its end, gone one tick later.
    history.Note(1000 + kKeep, false);
    REQUIRE(history.Size() == 1);
    history.Note(1000 + kKeep + 1, false);
    REQUIRE(history.Size() == 0);

    // A clock that went back forgets what it knew.
    History back;
    back.Note(5000, true);
    back.Note(5100, true);
    back.Note(4000, true);
    REQUIRE(back.Size() == 1);
    REQUIRE_FALSE(back.HeldDuring(5000, 5100));
    REQUIRE(back.HeldDuring(4000, 4000));

    REQUIRE(SeenAfter(10000, 10000 - 15) == kMinSeenAfter);
    REQUIRE(SeenAfter(10000, 10000 - 17) == 17);
    REQUIRE(SeenAfter(10000, 10000 - 399) == 399);
    REQUIRE(SeenAfter(10000, 10000 - 401) == kMaxSeenAfter);

    // Not a number is never faced.
    const float cNan = std::nanf("");
    REQUIRE_FALSE(Faces(0.f, 0.f, cNan, 0.f, 100.f));
    REQUIRE_FALSE(Faces(cNan, 0.f, 0.f, 0.f, 100.f));
    // Headings past a turn are the same facing.
    REQUIRE(Faces(0.f, 0.f, 2.f * 3.14159265f, 0.f, 100.f));
    REQUIRE(Faces(0.f, 0.f, -3.14159265f / 2.f, -100.f, 0.f));
}
