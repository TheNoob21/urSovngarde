#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <PlayerGraphEvents.h>
#include <Structs/ActionEvent.h>

using namespace PlayerGraphEvents;

namespace
{
const Clock::time_point s_start{};

Clock::time_point At(const int aMilliseconds)
{
    return s_start + std::chrono::milliseconds(aMilliseconds);
}
} // namespace

TEST_CASE("Only the allowed idles are a player's graph events to send, by any case", "[player_graph_events]")
{
    CHECK(IsAllowed("IdleWave"));
    CHECK(IsAllowed("idlewave"));
    CHECK(IsAllowed("IDLESTOP"));
    CHECK(IsAllowed("IdleForceDefaultState"));
    CHECK(IsAllowed("IdleCiceroDance1"));

    // The game's own actions and anything not on the list.
    CHECK_FALSE(IsAllowed("attackStart"));
    CHECK_FALSE(IsAllowed("SneakStart"));
    CHECK_FALSE(IsAllowed("staggerStart"));
    CHECK_FALSE(IsAllowed("Ragdoll"));
    CHECK_FALSE(IsAllowed("IdleWav"));
    CHECK_FALSE(IsAllowed("IdleWaveX"));
    CHECK_FALSE(IsAllowed(""));

    // Every name is found at its own place.
    for (std::size_t i = 0; i < kAllowedCount; ++i)
    {
        REQUIRE(IndexOf(kAllowed[i]).has_value());
        CHECK(static_cast<std::size_t>(*IndexOf(kAllowed[i])) == i);
    }
}

TEST_CASE("A graph event is told from the game's actions by its Type and its missing action", "[player_graph_events]")
{
    CHECK(IsGraphEvent(kTypeBit, 0));
    // The game's own Type: its action input and bit 2.
    CHECK_FALSE(IsGraphEvent(0, 0));
    CHECK_FALSE(IsGraphEvent(0x7, 0));
    // A server's instant counterpart has no action either, and keeps the game's Type.
    CHECK_FALSE(IsGraphEvent(0x1, 0));
    // An action with a form is the game's, whatever its Type.
    CHECK_FALSE(IsGraphEvent(kTypeBit, 0x13005));

    // It goes over as the action message already does.
    ActionEvent sent;
    sent.Type = kTypeBit;
    sent.Tick = 100;
    sent.EventName = "IdleWave";

    Buffer buffer(1000);
    Buffer::Writer writer(&buffer);
    sent.GenerateDifferential(ActionEvent{}, writer);
    Buffer::Reader reader(&buffer);
    ActionEvent received;
    received.ApplyDifferential(reader);

    REQUIRE(received == sent);
    CHECK(IsGraphEvent(received.Type, received.ActionId));
    CHECK(IsAllowed(received.EventName));
}

TEST_CASE("At most four graph events a second are let through", "[player_graph_events]")
{
    RateLimit rate;
    CHECK(rate.Take(At(0)));
    CHECK(rate.Take(At(10)));
    CHECK(rate.Take(At(20)));
    CHECK(rate.Take(At(30)));
    CHECK_FALSE(rate.Take(At(40)));
    CHECK_FALSE(rate.Take(At(999)));
    // A second after the first, one more.
    CHECK(rate.Take(At(1000)));
    CHECK_FALSE(rate.Take(At(1005)));
    CHECK(rate.Take(At(1010)));
    CHECK(rate.Take(At(1020)));
    CHECK(rate.Take(At(1030)));
    CHECK_FALSE(rate.Take(At(1040)));
    // After a pause, four again.
    CHECK(rate.Take(At(5000)));
    CHECK(rate.Take(At(5001)));
    CHECK(rate.Take(At(5002)));
    CHECK(rate.Take(At(5003)));
    CHECK_FALSE(rate.Take(At(5004)));
}

TEST_CASE("The outbox keeps allowed events in order, within the rate and its size", "[player_graph_events]")
{
    Outbox outbox;
    CHECK_FALSE(outbox.Push("attackStart", At(0)));
    CHECK_FALSE(outbox.Push("staggerStart", At(0)));
    // A refused name uses none of the rate.
    CHECK(outbox.Push("idlewave", At(0)));
    CHECK(outbox.Push("IdleSalute", At(1)));
    CHECK(outbox.Push("IdleLaugh", At(2)));
    CHECK(outbox.Push("IdleStop", At(3)));
    CHECK_FALSE(outbox.Push("IdleWave", At(4)));
    CHECK(outbox.Size() == 4);

    // Handed over in the list's own spelling.
    CHECK(outbox.Pop() == std::optional<std::string_view>("IdleWave"));
    CHECK(outbox.Pop() == std::optional<std::string_view>("IdleSalute"));
    CHECK(outbox.Pop() == std::optional<std::string_view>("IdleLaugh"));
    CHECK(outbox.Pop() == std::optional<std::string_view>("IdleStop"));
    CHECK_FALSE(outbox.Pop().has_value());
}

TEST_CASE("The outbox holds at most eight events when none are taken", "[player_graph_events]")
{
    Outbox outbox;
    int pushed = 0;
    // Four a second for three seconds, none taken out.
    for (int ms = 0; ms < 3000; ms += 250)
        if (outbox.Push("IdleWave", At(ms)))
            ++pushed;
    CHECK(pushed == static_cast<int>(kMaxQueued));
    CHECK(outbox.Size() == kMaxQueued);

    // A full outbox takes no rate either: once emptied, events go again at once.
    while (outbox.Pop())
    {
    }
    CHECK(outbox.Push("IdleStop", At(3000)));
}

TEST_CASE("An event that ends a gesture goes only shortly after a gesture, once", "[player_graph_events]")
{
    using namespace PlayerGraphEvents;
    const auto cStart = Clock::now();
    EndGate gate;
    // A reset a mod sends on its own is not passed on: on a copy it would stand it up from a seat or a horse.
    CHECK_FALSE(gate.Allows("IdleStop", cStart));
    CHECK_FALSE(gate.Allows("IdleForceDefaultState", cStart));
    CHECK(gate.Allows("IdleWave", cStart));
    CHECK(gate.Allows("idlestop", cStart + std::chrono::seconds(3)));
    CHECK_FALSE(gate.Allows("IdleStopInstant", cStart + std::chrono::seconds(4)));
    // Too long after the gesture.
    CHECK(gate.Allows("IdlePray", cStart + std::chrono::seconds(20)));
    CHECK_FALSE(gate.Allows("IdleStop", cStart + std::chrono::seconds(20) + kEndWithin + std::chrono::seconds(1)));
    CHECK_FALSE(gate.Allows("NotAnIdle", cStart));
}

TEST_CASE("The outbox holds back an end event with no gesture before it", "[player_graph_events]")
{
    using namespace PlayerGraphEvents;
    const auto cStart = Clock::now();
    Outbox outbox;
    CHECK_FALSE(outbox.Push("IdleForceDefaultState", cStart));
    CHECK(outbox.Push("IdleSalute", cStart));
    CHECK(outbox.Push("IdleStop", cStart + std::chrono::milliseconds(500)));
    CHECK(outbox.Size() == 2);
}
