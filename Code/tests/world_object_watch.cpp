#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>

#include <catch2/catch.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include <Services/WorldObjectWatch.h>
#include <Services/HeldObjectPlayback.h>
#include <ObjectMover.h>

using namespace WorldObjectWatch;
using namespace std::chrono_literals;

TEST_CASE("a neighbour cell is scanned only when it reaches within the watched distance", "[worldobjects]")
{
    // Cell (0, 0) spans 0..4096 on both axes.
    const glm::vec2 cMiddle(2048.f, 2048.f);
    CHECK(IsCellNear(0, 0, cMiddle, 800.f));
    CHECK_FALSE(IsCellNear(1, 0, cMiddle, 800.f));
    CHECK_FALSE(IsCellNear(-1, -1, cMiddle, 800.f));

    // Near the east edge: the cell to the east is in reach, the one north-east only near the corner.
    const glm::vec2 cEastEdge(4000.f, 2048.f);
    CHECK(IsCellNear(1, 0, cEastEdge, 800.f));
    CHECK_FALSE(IsCellNear(1, 1, cEastEdge, 800.f));
    const glm::vec2 cCorner(4000.f, 4000.f);
    CHECK(IsCellNear(1, 1, cCorner, 800.f));

    // Negative cells: (-1, 0) spans -4096..0.
    CHECK(IsCellNear(-1, 0, glm::vec2(100.f, 50.f), 400.f));
    CHECK_FALSE(IsCellNear(-1, 0, glm::vec2(500.f, 50.f), 400.f));
}

TEST_CASE("an object is as near as the nearest player", "[worldobjects]")
{
    const glm::vec3 cPlayer(0.f);
    CHECK(Nearness(glm::vec3(300.f, 0.f, 0.f), cPlayer, {}) == Approx(300.f));
    CHECK(Nearness(glm::vec3(300.f, 0.f, 0.f), cPlayer, {glm::vec3(350.f, 0.f, 0.f)}) == Approx(50.f));
    CHECK(Nearness(glm::vec3(300.f, 0.f, 0.f), cPlayer, {glm::vec3(-900.f, 0.f, 0.f)}) == Approx(300.f));
    CHECK(std::isinf(Nearness(glm::vec3(std::numeric_limits<float>::quiet_NaN()), cPlayer, {})));
}

TEST_CASE("the watch list takes the nearest objects of all cells, each once, up to its budget", "[worldobjects]")
{
    SECTION("nearest first, whatever cell lists them first")
    {
        const auto cPicked = Pick({{1, 390.f}, {2, 20.f}, {3, 150.f}, {4, 700.f}});
        CHECK(cPicked == std::vector<uint32_t>{2, 3, 1, 4});
    }

    SECTION("cut at the budget, keeping the nearest")
    {
        std::vector<Candidate> found;
        // The cell lists the far ones first: in list order the near ones would have been left out.
        for (uint32_t i = 0; i < 200; ++i)
            found.push_back({1000 + i, 399.f - static_cast<float>(i)});
        const auto cPicked = Pick(found);
        REQUIRE(cPicked.size() == kMaxWatched);
        CHECK(cPicked.front() == 1199);
        CHECK(cPicked.back() == 1199 - (kMaxWatched - 1));
    }

    SECTION("an object found twice is watched once, at its nearest")
    {
        const auto cPicked = Pick({{7, 300.f}, {8, 100.f}, {7, 50.f}}, 2);
        CHECK(cPicked == std::vector<uint32_t>{7, 8});
    }

    SECTION("a distance that is no number goes last")
    {
        const auto cPicked = Pick({{1, std::numeric_limits<float>::quiet_NaN()}, {2, 300.f}});
        CHECK(cPicked == std::vector<uint32_t>{2, 1});
    }

    SECTION("equal distances keep one order")
    {
        CHECK(Pick({{9, 10.f}, {3, 10.f}}) == std::vector<uint32_t>{3, 9});
    }
}

TEST_CASE("objects just watched that start together are their cell settling", "[worldobjects]")
{
    const auto cAt = Clock::time_point{} + 100s;
    CHECK(IsSettling(cAt, cAt));
    CHECK(IsSettling(cAt - kSettleFor + 1ms, cAt));
    CHECK_FALSE(IsSettling(cAt - kSettleFor, cAt));
    // Watched since before anything else: never settling.
    CHECK_FALSE(IsSettling(Clock::time_point{}, cAt));
}

TEST_CASE("one game sends each start: in a blast the nearer player's by a margin, else the lower id's", "[worldobjects]")
{
    SECTION("a hand's start is theirs only right next to their copy")
    {
        CHECK(IsTheirs(300.f, 5, {{100.f, 9}}, false));
        CHECK_FALSE(IsTheirs(300.f, 5, {{200.f, 1}}, false));
        CHECK_FALSE(IsTheirs(80.f, 5, {{100.f, 1}}, false));
        CHECK_FALSE(IsTheirs(300.f, 5, {}, false));
    }

    SECTION("a blast's object is theirs when their copy is nearer by more than the margin")
    {
        CHECK(IsTheirs(300.f, 1, {{300.f - kClaimMargin - 1.f, 2}}, true));
        CHECK(IsTheirs(900.f, 1, {{600.f, 2}, {1200.f, 3}}, true));
        CHECK_FALSE(IsTheirs(300.f, 2, {{300.f + kClaimMargin + 1.f, 1}}, true));
    }

    SECTION("within the margin the lower id sends, whoever stands nearer")
    {
        CHECK(IsTheirs(300.f, 2, {{340.f, 1}}, true));
        CHECK_FALSE(IsTheirs(340.f, 1, {{300.f, 2}}, true));
        CHECK(IsTheirs(300.f, 5, {{300.f, 3}}, true));
        CHECK_FALSE(IsTheirs(300.f, 3, {{300.f, 5}}, true));
        CHECK_FALSE(IsTheirs(300.f, 3, {{std::numeric_limits<float>::quiet_NaN(), 1}}, true));
    }
}

TEST_CASE("a blast object left by both games is sent after a second by a game that still sees it move", "[worldobjects]")
{
    const auto cLeft = Clock::time_point{} + 100s;
    CHECK_FALSE(ClaimsLeftObject(Clock::time_point{}, Clock::time_point{}, cLeft + 2s));
    CHECK_FALSE(ClaimsLeftObject(cLeft, Clock::time_point{}, cLeft + 900ms));
    CHECK(ClaimsLeftObject(cLeft, Clock::time_point{}, cLeft + kClaimAfter));
    CHECK(ClaimsLeftObject(cLeft, cLeft - 5s, cLeft + 2s));
    // A move of it came from the game it was left to: that game sends it.
    CHECK_FALSE(ClaimsLeftObject(cLeft, cLeft + 100ms, cLeft + 2s));
    // Long after the blast it is not claimed from an old leaving.
    CHECK_FALSE(ClaimsLeftObject(cLeft, Clock::time_point{}, cLeft + kClaimUntil));
}

TEST_CASE("a resting object left in a blast is sent once where it lies when no move of it came", "[worldobjects]")
{
    const auto cLeft = Clock::time_point{} + 100s;
    CHECK(JudgeResting(true, Clock::time_point{}, Clock::time_point{}, cLeft) == Resting::Starts);
    CHECK(JudgeResting(true, cLeft, Clock::time_point{}, cLeft + 2s) == Resting::Starts);
    CHECK(JudgeResting(false, Clock::time_point{}, Clock::time_point{}, cLeft + 2s) == Resting::Stays);
    // Landed within its quiet: once the quiet is over, it is sent from here.
    CHECK(JudgeResting(false, cLeft, Clock::time_point{}, cLeft + kClaimAfter) == Resting::SendsPlace);
    CHECK(JudgeResting(false, cLeft, Clock::time_point{}, cLeft + 900ms) == Resting::Stays);
    // A move of it came from there: that game sends it.
    CHECK(JudgeResting(false, cLeft, cLeft + 200ms, cLeft + 2s) == Resting::Stays);
    CHECK(JudgeResting(false, cLeft, Clock::time_point{}, cLeft + kClaimUntil) == Resting::Stays);
}

namespace
{
// One game's copy of a blast object, and the watch pass DroppedItemService runs on it each 33 ms.
struct BlastGame
{
    uint32_t Id{};
    //! What IsTheirs says when its copy starts.
    bool Leaves{};
    //! When its copy starts and stops moving, after the blast. A copy that never moves here has no speed.
    Clock::duration StartAfter{};
    Clock::duration LandAfter{};
    float Speed{300.f};

    Clock::time_point QuietUntil{};
    Clock::time_point LeftAt{};
    Clock::time_point LastRemote{};
    Clock::time_point LastLocalMove{};
    float LastPosition{};
    bool Moving{};
    bool Held{};
    size_t Starts{};
    size_t Rests{};

    [[nodiscard]] float PositionAt(const Clock::time_point acBlast, const Clock::time_point acNow) const noexcept
    {
        const Clock::duration cMoved = std::clamp(acNow - acBlast - StartAfter, Clock::duration::zero(), LandAfter - StartAfter);
        return Speed * std::chrono::duration<float>(cMoved).count();
    }
};

// A move on its way: to the server from game Game, or from the server to game Game.
struct BlastSend
{
    Clock::time_point At;
    size_t Game;
    bool AtRest;
};

struct BlastOutcome
{
    //! What the server passed on, by sender.
    size_t PassedRests[2]{};
    size_t PassedMoves[2]{};
};

BlastOutcome PlayBlast(std::array<BlastGame, 2>& aGames, const Clock::duration acLatency)
{
    const auto cBlast = Clock::time_point{} + 100s;
    std::vector<BlastSend> toServer;
    std::vector<BlastSend> toGames;
    ObjectMover mover;
    BlastOutcome outcome;
    for (auto now = cBlast; now < cBlast + 6s; now += 33ms)
    {
        for (const BlastSend& cSend : toServer)
        {
            if (cSend.At > now || !mover.Take(aGames[cSend.Game].Id, cSend.AtRest, cSend.At))
                continue;
            if (cSend.AtRest)
                ++outcome.PassedRests[cSend.Game];
            else
                ++outcome.PassedMoves[cSend.Game];
            toGames.push_back({cSend.At + acLatency, 1 - cSend.Game, cSend.AtRest});
        }
        std::erase_if(toServer, [now](const BlastSend& acSend) { return acSend.At <= now; });

        // A move from there: held and followed, and its leaving forgotten (OnNotifyWorldObjectMove).
        for (const BlastSend& cSend : toGames)
        {
            if (cSend.At > now)
                continue;
            BlastGame& game = aGames[cSend.Game];
            game.Held = true;
            game.Moving = false;
            game.LeftAt = {};
            game.LastRemote = cSend.At;
        }
        std::erase_if(toGames, [now](const BlastSend& acSend) { return acSend.At <= now; });

        for (size_t i = 0; i < aGames.size(); ++i)
        {
            BlastGame& game = aGames[i];
            if (game.Held)
                continue;
            const float cPosition = game.PositionAt(cBlast, now);
            const auto send = [&](const bool aAtRest)
            {
                toServer.push_back({now + acLatency, i, aAtRest});
            };
            if (now < game.QuietUntil)
            {
                game.LastPosition = cPosition;
                continue;
            }
            if (!game.Moving)
            {
                switch (JudgeResting(std::abs(cPosition - game.LastPosition) > 8.f, game.LeftAt, game.LastRemote, now))
                {
                case Resting::Starts:
                    game.LastPosition = cPosition;
                    if (game.Leaves && now - game.LastLocalMove >= 3s && !ClaimsLeftObject(game.LeftAt, game.LastRemote, now))
                    {
                        game.QuietUntil = now + 1s;
                        if (game.LeftAt == Clock::time_point{} || now - game.LeftAt >= kClaimUntil)
                            game.LeftAt = now;
                        break;
                    }
                    game.LastLocalMove = now;
                    game.Moving = true;
                    game.LeftAt = {};
                    ++game.Starts;
                    send(false);
                    break;
                case Resting::SendsPlace:
                    game.LastPosition = cPosition;
                    game.LeftAt = {};
                    ++game.Rests;
                    send(true);
                    break;
                default: break;
                }
                continue;
            }
            if (std::abs(cPosition - game.LastPosition) > 2.f)
            {
                game.LastPosition = cPosition;
                game.LastLocalMove = now;
                send(false);
            }
            else if (now - game.LastLocalMove >= 1s)
            {
                game.Moving = false;
                game.LastPosition = cPosition;
                ++game.Rests;
                send(true);
            }
        }
    }
    return outcome;
}

// One player's moves and exactly one rest passed on, and the other game follows them.
void CheckOneSender(const std::array<BlastGame, 2>& acGames, const BlastOutcome& acOutcome)
{
    CHECK(acOutcome.PassedRests[0] + acOutcome.PassedRests[1] == 1);
    const size_t cSender = acOutcome.PassedRests[0] == 1 ? 0 : 1;
    CHECK(acOutcome.PassedMoves[1 - cSender] == 0);
    CHECK_FALSE(acGames[cSender].Held);
    CHECK(acGames[1 - cSender].Held);
    CHECK(acGames[0].Rests <= 1);
    CHECK(acGames[1].Rests <= 1);
}
} // namespace

TEST_CASE("a blast object both games left gets one sender, whenever it lands", "[worldobjects]")
{
    SECTION("landing within a second after both left it, it is sent once at rest")
    {
        std::array<BlastGame, 2> games{BlastGame{1, true, 0ms, 800ms}, BlastGame{2, true, 0ms, 800ms}};
        const BlastOutcome cOutcome = PlayBlast(games, 40ms);
        CheckOneSender(games, cOutcome);
        CHECK(cOutcome.PassedMoves[0] + cOutcome.PassedMoves[1] == 0);
        CHECK(games[0].Starts + games[1].Starts == 0);
    }

    SECTION("the rest from the game that left it first arrives in time, and the other sends nothing")
    {
        std::array<BlastGame, 2> games{BlastGame{1, true, 0ms, 800ms}, BlastGame{2, true, 100ms, 800ms}};
        const BlastOutcome cOutcome = PlayBlast(games, 20ms);
        CheckOneSender(games, cOutcome);
        CHECK(cOutcome.PassedRests[0] == 1);
        CHECK(games[0].Rests == 1);
        CHECK(games[1].Rests == 0);
    }

    SECTION("still moving after a second, it is claimed as a start")
    {
        std::array<BlastGame, 2> games{BlastGame{1, true, 0ms, 1500ms}, BlastGame{2, true, 0ms, 1500ms}};
        const BlastOutcome cOutcome = PlayBlast(games, 40ms);
        CheckOneSender(games, cOutcome);
        CHECK(cOutcome.PassedMoves[0] + cOutcome.PassedMoves[1] > 0);
    }

    SECTION("the game it was left to sends it, and the game that left it sends nothing")
    {
        std::array<BlastGame, 2> games{BlastGame{1, false, 0ms, 800ms}, BlastGame{2, true, 0ms, 800ms}};
        const BlastOutcome cOutcome = PlayBlast(games, 40ms);
        CheckOneSender(games, cOutcome);
        CHECK(cOutcome.PassedRests[0] == 1);
        CHECK(games[1].Starts + games[1].Rests == 0);
    }

    SECTION("the game it was left to never saw it move: the game that left it sends where it lies")
    {
        std::array<BlastGame, 2> games{BlastGame{1, false, 0ms, 800ms, 0.f}, BlastGame{2, true, 0ms, 800ms}};
        const BlastOutcome cOutcome = PlayBlast(games, 40ms);
        CheckOneSender(games, cOutcome);
        CHECK(cOutcome.PassedRests[1] == 1);
        CHECK(games[0].Starts + games[0].Rests == 0);
    }
}

TEST_CASE("two games with their own late views of a blast agree on one sender, or one is found within a second", "[worldobjects]")
{
    // Game A (player id 1) at the origin, game B (player id 2) 600 units along x. Each game measures its own copy of the object,
    // which its physics has put a little apart, against where it shows the other player, a moment late.
    const glm::vec3 cA{0.f, 0.f, 0.f};
    const glm::vec3 cB{600.f, 0.f, 0.f};
    const std::vector<glm::vec3> cApart{{0.f, 0.f, 0.f}, {12.f, -8.f, 0.f}, {-10.f, 6.f, 3.f}};
    const std::vector<glm::vec3> cLate{{0.f, 0.f, 0.f}, {30.f, 0.f, 0.f}, {-25.f, 15.f, 0.f}};
    // The most either game's difference between the players' distances is off by: twice its object's offset, plus the late view.
    const float cMostOff = 2.f * 15.f + 30.f;

    const auto cDistanceTo = [](const glm::vec3& acFrom, const glm::vec3& acTo)
    {
        return glm::distance(acFrom, acTo);
    };
    size_t cases = 0;
    size_t agreed = 0;
    size_t fallbacks = 0;
    // Which of the four outcomes, by whether A and B left it, came up.
    bool seen[2][2]{};
    for (float x = -200.f; x <= 800.f; x += 4.f)
    {
        for (const float cY : {-150.f, 0.f, 150.f})
        {
            const glm::vec3 cObject{x, cY, 0.f};
            for (const glm::vec3& cApartA : cApart)
                for (const glm::vec3& cApartB : cApart)
                    for (const glm::vec3& cLateA : cLate)
                        for (const glm::vec3& cLateB : cLate)
                        {
                            const glm::vec3 cObjectInA = cObject + cApartA;
                            const glm::vec3 cObjectInB = cObject + cApartB;
                            const bool cLeftByA = IsTheirs(cDistanceTo(cObjectInA, cA), 1, {{cDistanceTo(cObjectInA, cB + cLateB), 2}}, true);
                            const bool cLeftByB = IsTheirs(cDistanceTo(cObjectInB, cB), 2, {{cDistanceTo(cObjectInB, cA + cLateA), 1}}, true);
                            ++cases;

                            // Away from the margin's edge, where B stands nearer by the margin, the games agree.
                            const float cCloserToB = cDistanceTo(cObject, cA) - cDistanceTo(cObject, cB);
                            if (std::abs(cCloserToB - kClaimMargin) > cMostOff)
                            {
                                CHECK(cLeftByA != cLeftByB);
                            }
                            if (cLeftByA != cLeftByB)
                                ++agreed;

                            if (cLeftByA && cLeftByB)
                                ++fallbacks;
                            seen[cLeftByA ? 1 : 0][cLeftByB ? 1 : 0] = true;
                        }
        }
    }
    CHECK(agreed > cases * 9 / 10);
    CHECK(fallbacks < cases / 10);
    CHECK(seen[1][1]);

    // Whatever each game decided, played out over the passes: one player's moves and one rest are passed on, whether the object
    // lands within the second both games may wait or moves on after it.
    for (const bool cLeftByA : {false, true})
        for (const bool cLeftByB : {false, true})
            for (const Clock::duration cLand : {Clock::duration(800ms), Clock::duration(1500ms)})
            {
                if (!seen[cLeftByA ? 1 : 0][cLeftByB ? 1 : 0])
                    continue;
                std::array<BlastGame, 2> games{BlastGame{1, cLeftByA, 0ms, cLand}, BlastGame{2, cLeftByB, 0ms, cLand}};
                const BlastOutcome cOutcome = PlayBlast(games, 40ms);
                CheckOneSender(games, cOutcome);
            }
}

TEST_CASE("a blast is sent a few at a time, within what the server passes on", "[worldobjects]")
{
    const auto cAt = Clock::time_point{} + 100s;

    SECTION("a few each pass")
    {
        Budget budget;
        CHECK(budget.Grant(cAt, 30) == Budget::kPerPass);
        CHECK(budget.Grant(cAt, 2) == 2);
        CHECK(budget.Grant(cAt, 0) == 0);
    }

    SECTION("a hand's moves are counted, and leave the blast less")
    {
        Budget budget;
        budget.Spend(cAt, Budget::kPerSecond - 1);
        CHECK(budget.Grant(cAt, 30) == 1);
        CHECK(budget.Grant(cAt + 33ms, 30) == 0);
        // A new second: the full pass again.
        CHECK(budget.Grant(cAt + 1s, 30) == Budget::kPerPass);
    }

    SECTION("thirty passes a second of a big blast stay under the server's 128 moves a second")
    {
        Budget budget;
        size_t total = 0;
        for (int pass = 0; pass < 30; ++pass)
            total += budget.Grant(cAt + pass * 33ms, 40);
        CHECK(total <= Budget::kPerSecond);
        CHECK(Budget::kPerSecond < 128);
        CHECK(total >= 60);
    }
}

TEST_CASE("dropped items knocked about by a shout share the blast's budget", "[worldobjects]")
{
    const auto cAt = Clock::time_point{} + 100s;

    SECTION("items count with the objects that start in the same pass")
    {
        CHECK(IsBlast(0, kManyStart));
        CHECK(IsBlast(2, kManyStart - 2));
        CHECK(IsBlast(kManyStart, 0));
        CHECK_FALSE(IsBlast(kManyStart - 2, 1));
        CHECK_FALSE(IsBlast(0, kManyStart - 1));
    }

    SECTION("six items moving every pass for two seconds stay within the budget, not 180 moves a second")
    {
        Budget budget;
        size_t perSecond[2]{};
        for (int pass = 0; pass < 60; ++pass)
        {
            const size_t cGranted = budget.Grant(cAt + pass * 33ms, 6);
            CHECK(cGranted <= Budget::kPerPass);
            perSecond[pass < 30 ? 0 : 1] += cGranted;
        }
        for (const size_t cSent : perSecond)
        {
            CHECK(cSent <= Budget::kPerSecond);
            CHECK(cSent < 6 * 30);
        }
    }

    SECTION("a hand carrying an item meanwhile is counted first")
    {
        Budget budget;
        size_t blast = 0;
        for (int pass = 0; pass < 30; ++pass)
        {
            budget.Spend(cAt + pass * 33ms, 1);
            blast += budget.Grant(cAt + pass * 33ms, 6);
        }
        // Four a pass fill the second's budget after 24 passes; the hand goes on, the blast waits.
        CHECK(blast == 24 * Budget::kPerPass);
        CHECK(blast + 24 == Budget::kPerSecond);
    }
}

TEST_CASE("a blast's sends go nearest first", "[worldobjects]")
{
    CHECK(Nearest({500.f, 30.f, 200.f, 90.f}, 2) == std::vector<size_t>{1, 3});
    CHECK(Nearest({500.f, 30.f}, 5) == std::vector<size_t>{1, 0});
    CHECK(Nearest({50.f, 50.f, 10.f}, 3) == std::vector<size_t>{2, 0, 1});
    CHECK(Nearest({std::numeric_limits<float>::quiet_NaN(), 10.f}, 1) == std::vector<size_t>{1});
    CHECK(Nearest({}, 3).empty());
}

TEST_CASE("a late move cannot undo the rest sent after it", "[worldobjects][heldobjects]")
{
    HeldObject::Order order;
    CHECK(order.Take(1000));
    CHECK(order.Take(1033));
    // The put-down, reliable, overtakes a move that went unreliable before it.
    CHECK(order.Take(1100));
    CHECK_FALSE(order.Take(1066));
    // The same update twice does no harm; the next carry goes on.
    CHECK(order.Take(1100));
    CHECK(order.Take(2000));

    // A resent resting place and a take carry no tick: always taken, and the order stays.
    CHECK(order.Take(0));
    CHECK_FALSE(order.Take(1999));
}
