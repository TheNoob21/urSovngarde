#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <Messages/ArrowNockedRequest.h>
#include <Messages/NotifyArrowNocked.h>
#include <Services/ArrowNocking.h>

#include <tuple>
#include <vector>

using namespace TiltedPhoques;

TEST_CASE("an arrow on the string keeps whose it is and whether it is there", "[arrow]")
{
    for (const bool nocked : {true, false})
    {
        ArrowNockedRequest request{};
        request.Nocked = nocked;

        Buffer buffer(32);
        Buffer::Writer writer(&buffer);
        request.SerializeRaw(writer);
        Buffer::Reader reader(&buffer);
        ArrowNockedRequest back{};
        back.DeserializeRaw(reader);
        REQUIRE(back == request);

        NotifyArrowNocked notify{};
        notify.ServerId = 0xFFFFFFFFu;
        notify.Nocked = nocked;

        Buffer notifyBuffer(32);
        Buffer::Writer notifyWriter(&notifyBuffer);
        notify.SerializeRaw(notifyWriter);
        Buffer::Reader notifyReader(&notifyBuffer);
        NotifyArrowNocked notifyBack{};
        notifyBack.DeserializeRaw(notifyReader);
        REQUIRE(notifyBack == notify);
    }
}

TEST_CASE("an arrow in the hand keeps which hand and where it sits", "[arrow]")
{
    ArrowNockedRequest request{};
    request.InHand = true;
    request.HandSide = 1;
    request.HandOffset = glm::vec3{-3.5f, 66.25f, 1.f / 3.f};
    request.HandRotation = glm::normalize(glm::quat{0.9f, 0.1f, -0.3f, 0.2f});

    Buffer buffer(64);
    Buffer::Writer writer(&buffer);
    request.SerializeRaw(writer);
    Buffer::Reader reader(&buffer);
    ArrowNockedRequest back{};
    back.DeserializeRaw(reader);
    REQUIRE(back == request);

    NotifyArrowNocked notify{};
    notify.ServerId = 0x12345u;
    notify.InHand = true;
    notify.HandSide = 0;
    notify.HandOffset = request.HandOffset;
    notify.HandRotation = request.HandRotation;

    Buffer notifyBuffer(64);
    Buffer::Writer notifyWriter(&notifyBuffer);
    notify.SerializeRaw(notifyWriter);
    Buffer::Reader notifyReader(&notifyBuffer);
    NotifyArrowNocked notifyBack{};
    notifyBack.DeserializeRaw(notifyReader);
    REQUIRE(notifyBack == notify);
    REQUIRE(notifyBack.HandSide == 0);
}

namespace
{
using namespace std::chrono_literals;

// Plays a list of (milliseconds, nocked, in hand) readings of the game with the bow out, and returns what was told after each.
std::vector<ArrowNocking::Arrow> Told(std::initializer_list<std::tuple<int, bool, bool>> aReadings)
{
    ArrowNocking::Filter filter;
    const auto cStart = std::chrono::steady_clock::now();
    std::vector<ArrowNocking::Arrow> told;
    for (const auto& [ms, nocked, inHand] : aReadings)
        told.push_back(filter.Update(nocked, inHand, true, cStart + std::chrono::milliseconds(ms)));
    return told;
}

constexpr ArrowNocking::Arrow cGone{false, false};
constexpr ArrowNocking::Arrow cInHand{false, true};
constexpr ArrowNocking::Arrow cOnString{true, false};
} // namespace

TEST_CASE("an arrow taken to the bow stays in the hand until it is on the string", "[arrow]")
{
    // A VR session: in the hand at 0, out of the hold as it touches the bow at 2425 ms, on the string at 3773 ms.
    const auto told = Told({{0, false, true}, {2425, false, false}, {3000, false, false}, {3700, false, false}, {3773, true, false}});
    CHECK(told[0] == cInHand);
    CHECK(told[1] == cInHand);
    CHECK(told[2] == cInHand);
    CHECK(told[3] == cInHand);
    CHECK(told[4] == cOnString);
}

TEST_CASE("an arrow that slips out of the hold for a moment is not told gone", "[arrow]")
{
    // A VR session: in the hand, out 44 ms later, back 210 ms after that.
    const auto told = Told({{0, false, true}, {44, false, false}, {254, false, true}});
    for (const auto& arrow : told)
        CHECK(arrow == cInHand);
}

TEST_CASE("an arrow put away is told gone once the hand stays empty", "[arrow]")
{
    const auto told = Told({{0, false, true}, {100, false, false}, {100 + 1499, false, false}, {100 + 1500, false, false}});
    CHECK(told[1] == cInHand);
    CHECK(told[2] == cInHand);
    CHECK(told[3] == cGone);
}

TEST_CASE("a shot arrow is gone at once", "[arrow]")
{
    const auto told = Told({{0, true, false}, {400, false, false}});
    CHECK(told[0] == cOnString);
    CHECK(told[1] == cGone);
}

TEST_CASE("an arrow is gone at once when the bow is put away", "[arrow]")
{
    ArrowNocking::Filter filter;
    const auto cStart = std::chrono::steady_clock::now();
    CHECK(filter.Update(false, true, true, cStart) == cInHand);
    CHECK(filter.Update(false, false, false, cStart + 10ms) == cGone);
}

TEST_CASE("the wait starts again each time the arrow comes back to the hand", "[arrow]")
{
    const auto told = Told({{0, false, true}, {100, false, false}, {1000, false, true}, {1100, false, false}, {2500, false, false}, {2600, false, false}});
    CHECK(told[4] == cInHand);
    CHECK(told[5] == cGone);
}

namespace
{
// As Told, with an arrow that left the hand taken as on the string: (milliseconds, nocked, in hand, shot) readings with the bow out.
std::vector<ArrowNocking::Arrow> ToldOnString(std::initializer_list<std::tuple<int, bool, bool, bool>> aReadings)
{
    ArrowNocking::Filter filter;
    const auto cStart = std::chrono::steady_clock::now();
    std::vector<ArrowNocking::Arrow> told;
    for (const auto& [ms, nocked, inHand, shot] : aReadings)
        told.push_back(filter.Update(nocked, inHand, true, cStart + std::chrono::milliseconds(ms), shot, 20.f));
    return told;
}
} // namespace

TEST_CASE("a PCVR draw keeps the arrow on the string until it is shot", "[arrow]")
{
    // The PCVR session of 2026-11-18: attack state 0 throughout, in the hand at 0, out of the hold at the nock, shot 3.4 s later.
    const auto told = ToldOnString(
        {{0, false, true, false},
         {700, false, false, false},
         {1199, false, false, false},
         {1200, false, false, false},
         {2800, false, false, false},
         {4100, false, false, false},
         {4110, false, false, true}});
    CHECK(told[0] == cInHand);
    CHECK(told[1] == cInHand);
    CHECK(told[2] == cInHand);
    CHECK(told[3] == cOnString);
    // Past the 1.5 s after which the hand used to be told empty.
    CHECK(told[4] == cOnString);
    CHECK(told[5] == cOnString);
    CHECK(told[6] == cGone);
}

TEST_CASE("an arrow that slips out of the hold for a moment stays in the hand while the bow is out", "[arrow]")
{
    const auto told = ToldOnString({{0, false, true, false}, {44, false, false, false}, {254, false, true, false}});
    for (const auto& arrow : told)
        CHECK(arrow == cInHand);
}

TEST_CASE("a shot before the arrow is taken as on the string leaves nothing", "[arrow]")
{
    const auto told = ToldOnString({{0, false, true, false}, {100, false, false, false}, {300, false, false, true}, {2000, false, false, false}});
    CHECK(told[1] == cInHand);
    CHECK(told[2] == cGone);
    CHECK(told[3] == cGone);
}

TEST_CASE("the next arrow in the hand after a shot is told in the hand", "[arrow]")
{
    // A shot and the next arrow from the quiver in the same reading.
    const auto told = ToldOnString({{0, false, true, false}, {100, false, false, false}, {600, false, false, false}, {1000, false, true, true}});
    CHECK(told[2] == cOnString);
    CHECK(told[3] == cInHand);
}

TEST_CASE("the game's attack state still says when the arrow leaves the string", "[arrow]")
{
    // A Quest draw: the attack state puts it on the string, and it is gone when the state drops, with or without the shot seen.
    const auto told = ToldOnString({{0, false, true, false}, {2425, false, false, false}, {3773, true, false, false}, {5000, false, false, false}, {5100, false, false, false}});
    CHECK(told[1] == cInHand);
    CHECK(told[2] == cOnString);
    CHECK(told[3] == cGone);
    CHECK(told[4] == cGone);
}

TEST_CASE("an arrow on the string is gone at once when the bow is put away", "[arrow]")
{
    ArrowNocking::Filter filter;
    const auto cStart = std::chrono::steady_clock::now();
    CHECK(filter.Update(false, true, true, cStart) == cInHand);
    CHECK(filter.Update(false, false, true, cStart + 100ms, false, 20.f) == cInHand);
    CHECK(filter.Update(false, false, true, cStart + 700ms, false, 20.f) == cOnString);
    CHECK(filter.Update(false, false, false, cStart + 800ms) == cGone);
    CHECK(filter.Update(false, false, true, cStart + 900ms) == cGone);
}

namespace
{
// As ToldOnString, with how far apart the hands were at each reading: (milliseconds, nocked, in hand, shot, hands apart).
std::vector<ArrowNocking::Arrow> ToldWithHands(std::initializer_list<std::tuple<int, bool, bool, bool, float>> aReadings)
{
    ArrowNocking::Filter filter;
    const auto cStart = std::chrono::steady_clock::now();
    std::vector<ArrowNocking::Arrow> told;
    for (const auto& [ms, nocked, inHand, shot, apart] : aReadings)
        told.push_back(filter.Update(nocked, inHand, true, cStart + std::chrono::milliseconds(ms), shot, apart));
    return told;
}
} // namespace

TEST_CASE("a hold that slips out with the hands at the bow puts the arrow on the string", "[arrow]")
{
    const auto told = ToldWithHands(
        {{0, false, true, false, 60.f}, {100, false, false, false, 20.f}, {400, false, false, false, 18.f}, {600, false, false, false, 30.f}, {2000, false, false, false, 55.f}});
    CHECK(told[1] == cInHand);
    CHECK(told[2] == cInHand);
    CHECK(told[3] == cOnString);
    CHECK(told[4] == cOnString);
}

TEST_CASE("a hold that slips out far from the bow stays in the hand, then is gone", "[arrow]")
{
    // The hand lowered to the hip: never near the bow, so no nock; the old grace applies.
    const auto told = ToldWithHands(
        {{0, false, true, false, 60.f},
         {100, false, false, false, 70.f},
         {700, false, false, false, 75.f},
         {1599, false, false, false, 80.f},
         {1600, false, false, false, 80.f},
         {1700, false, false, false, 80.f}});
    CHECK(told[1] == cInHand);
    CHECK(told[2] == cInHand);
    CHECK(told[3] == cInHand);
    CHECK(told[4] == cGone);
    CHECK(told[5] == cGone);
}

TEST_CASE("a hand that reached the bow and then drew back keeps the nock", "[arrow]")
{
    // At the bow at 200 ms, the draw takes the hands 60 units apart by 700 ms.
    const auto told = ToldWithHands({{0, false, true, false, 60.f}, {100, false, false, false, 50.f}, {200, false, false, false, 30.f}, {700, false, false, false, 60.f}});
    CHECK(told[3] == cOnString);
}

TEST_CASE("a hold that slips out for a moment near the bow is not yet the string", "[arrow]")
{
    const auto told = ToldWithHands({{0, false, true, false, 20.f}, {44, false, false, false, 20.f}, {254, false, true, false, 20.f}});
    for (const auto& arrow : told)
        CHECK(arrow == cInHand);
}

TEST_CASE("the nock is forgotten when the hands part for good without a shot", "[arrow]")
{
    const auto told = ToldWithHands(
        {{0, false, true, false, 60.f},
         {100, false, false, false, 20.f},
         {700, false, false, false, 20.f},
         {1000, false, false, false, 70.f},
         {2000, false, false, false, 120.f},
         {2900, false, false, false, 120.f},
         {3000, false, false, false, 120.f}});
    CHECK(told[2] == cOnString);
    CHECK(told[3] == cOnString);
    CHECK(told[4] == cOnString);
    CHECK(told[5] == cOnString);
    CHECK(told[6] == cGone);
}

TEST_CASE("a draw that comes back within reach is not let go", "[arrow]")
{
    const auto told = ToldWithHands(
        {{0, false, true, false, 60.f},
         {100, false, false, false, 20.f},
         {700, false, false, false, 20.f},
         {1000, false, false, false, 120.f},
         {1800, false, false, false, 60.f},
         {2900, false, false, false, 120.f}});
    CHECK(told[2] == cOnString);
    CHECK(told[4] == cOnString);
    CHECK(told[5] == cOnString);
}

TEST_CASE("unknown hand distance preserves the hand grace without asserting a nock", "[arrow]")
{
    const auto told = ToldWithHands({{0, false, true, false, -1.f}, {100, false, false, false, -1.f}, {700, false, false, false, -1.f}, {4000, false, false, false, -1.f}});
    CHECK(told[1] == cInHand);
    CHECK(told[2] == cInHand);
    CHECK(told[3] == cGone);
}

TEST_CASE("an inferred nock waits for the hands to be tracked again", "[arrow]")
{
    const auto told = ToldWithHands(
        {{0, false, true, false, 60.f},
         {100, false, false, false, 20.f},
         {650, false, false, false, -1.f},
         {800, false, false, false, 60.f},
         {950, false, false, false, -1.f},
         {1000, false, false, true, -1.f}});
    CHECK(told[2] == cInHand);
    CHECK(told[3] == cOnString);
    CHECK(told[4] == cOnString);
    CHECK(told[5] == cGone);
}

TEST_CASE("an old bow observation cannot assert a nock after tracking loss", "[arrow]")
{
    const auto told = ToldWithHands({{0, false, true, false, 60.f}, {100, false, false, false, 20.f}, {650, false, false, false, -1.f}, {1600, false, false, false, 60.f}});
    CHECK(told[2] == cInHand);
    CHECK(told[3] == cGone);
}

TEST_CASE("a quick draw can use the last tracked in-hand bow contact", "[arrow]")
{
    const auto told = ToldWithHands(
        {{0, false, true, false, 20.f}, {100, false, false, false, 60.f}, {600, false, false, false, 60.f}, {700, false, false, false, -1.f}, {2200, false, false, false, -1.f}});
    CHECK(told[1] == cInHand);
    CHECK(told[2] == cOnString);
    CHECK(told[3] == cOnString);
    CHECK(told[4] == cGone);
}
