#include <catch2/catch.hpp>

#include <Systems/PendingVoice.h>

using namespace std::chrono_literals;

TEST_CASE("An NPC line waits briefly for its speaker and its lip file", "[dialogue]")
{
    const PendingVoice::Clock::time_point heard{};
    PendingVoice voice{heard};

    CHECK_FALSE(voice.Expired(heard + 2999ms));
    CHECK(voice.Expired(heard + 3s));
    CHECK_FALSE(voice.WaitingForLip(heard + 1s));

    voice.FirstAsked = heard + 2500ms;
    CHECK(voice.WaitingForLip(heard + 3s));
    CHECK_FALSE(voice.Expired(heard + 3s));
    CHECK_FALSE(voice.WaitingForLip(heard + 3500ms));
    CHECK_FALSE(voice.Expired(heard + 5499ms));
    CHECK(voice.Expired(heard + 5500ms));
}
