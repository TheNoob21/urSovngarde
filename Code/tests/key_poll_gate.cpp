#include <catch2/catch.hpp>

#include <Services/KeyPollGate.h>

using Clock = KeyPollGate::Clock;
using namespace std::chrono_literals;

TEST_CASE("The first frame reads the keys", "[keypoll]")
{
    KeyPollGate gate;
    REQUIRE_FALSE(gate.IsOpen());

    gate.Advance(Clock::now());
    REQUIRE(gate.IsOpen());
}

TEST_CASE("The keys are read once per interval, not on every frame", "[keypoll]")
{
    KeyPollGate gate;
    const auto t0 = Clock::now();

    int reads = 0;
    for (int i = 0; i < 60; ++i) // one second at 60 fps
    {
        gate.Advance(t0 + i * 16ms);
        reads += gate.IsOpen() ? 1 : 0;
    }

    // Frames at 0, 112, 224, ... ms: one in every seven.
    REQUIRE(reads == 9);
}

TEST_CASE("A frame closer than the interval to the last read does not read", "[keypoll]")
{
    KeyPollGate gate;
    const auto t0 = Clock::now();

    gate.Advance(t0);
    REQUIRE(gate.IsOpen());

    gate.Advance(t0 + KeyPollGate::cInterval - 1ms);
    REQUIRE_FALSE(gate.IsOpen());

    gate.Advance(t0 + KeyPollGate::cInterval);
    REQUIRE(gate.IsOpen());
}

TEST_CASE("The interval counts from the last read, not from the last frame", "[keypoll]")
{
    KeyPollGate gate;
    const auto t0 = Clock::now();

    gate.Advance(t0);
    // Frames that do not read must not push the next read back.
    gate.Advance(t0 + 60ms);
    REQUIRE_FALSE(gate.IsOpen());
    gate.Advance(t0 + 99ms);
    REQUIRE_FALSE(gate.IsOpen());
    gate.Advance(t0 + 100ms);
    REQUIRE(gate.IsOpen());
}

TEST_CASE("A slow frame reads, and the next interval starts there", "[keypoll]")
{
    KeyPollGate gate;
    const auto t0 = Clock::now();

    gate.Advance(t0);
    gate.Advance(t0 + 2s);
    REQUIRE(gate.IsOpen());

    gate.Advance(t0 + 2s + 50ms);
    REQUIRE_FALSE(gate.IsOpen());
}

TEST_CASE("Every reader of the same frame gets the same answer", "[keypoll]")
{
    KeyPollGate gate;
    const auto t0 = Clock::now();

    gate.Advance(t0);
    // DebugService and MagicService both ask in this frame.
    REQUIRE(gate.IsOpen());
    REQUIRE(gate.IsOpen());

    gate.Advance(t0 + 16ms);
    REQUIRE_FALSE(gate.IsOpen());
    REQUIRE_FALSE(gate.IsOpen());
}

TEST_CASE("A clock that moves back cannot keep the gate shut", "[keypoll]")
{
    KeyPollGate gate;
    const auto t0 = Clock::now();

    gate.Advance(t0 + 10s);
    gate.Advance(t0);
    REQUIRE(gate.IsOpen());
}
