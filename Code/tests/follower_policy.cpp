#include <catch2/catch.hpp>

#include <FollowerPolicy.h>
#include <GameApi.h>

using namespace FollowerPolicy;

TEST_CASE("an add-on's follower answers are merged with core's own rule", "[followers]")
{
    SECTION("with no add-on, or none deciding, core's rule stands as before")
    {
        CHECK(Merge(nullptr, 0) == kDefer);
        const uint32_t cDeferring[] = {0, 0};
        CHECK(Merge(cDeferring, 2) == kDefer);
        CHECK(Decide(Merge(cDeferring, 2), true));
        CHECK_FALSE(Decide(Merge(cDeferring, 2), false));
    }

    SECTION("an add-on's yes or no decides, the first in registration order")
    {
        const uint32_t cYesThenNo[] = {0, kFollower, kNotFollower};
        CHECK(Merge(cYesThenNo, 3) == kFollower);
        CHECK(Decide(kFollower, false));
        CHECK_FALSE(Decide(kNotFollower, true));
    }

    SECTION("an answer that is neither yes nor no defers")
    {
        const uint32_t cOdd[] = {7, kNotFollower};
        CHECK(Merge(cOdd, 2) == kNotFollower);
        const uint32_t cAllOdd[] = {3, 9};
        CHECK(Merge(cAllOdd, 2) == kDefer);
    }

    SECTION("a word is handled only with a follower's reply: done, busy or gone")
    {
        CHECK(WordReply(0) == 0);
        CHECK(WordReply(1) == 1);
        CHECK(WordReply(2) == 2);
        CHECK(WordReply(3) == 0);
        CHECK(WordReply(4) == 4);
        CHECK(WordReply(5) == 0);
    }
}

TEST_CASE("an add-on calls RegisterFollowerPolicy only when the core's API reaches it", "[followers]")
{
    UrGameApi api{};
    api.Version = UR_GAME_API_VERSION;

    api.Size = sizeof(UrGameApi);
    CHECK(UR_GAME_API_HAS(&api, RegisterFollowerPolicy));

    // An older core: its table ends before the member.
    api.Size = static_cast<uint32_t>(offsetof(UrGameApi, RegisterFollowerPolicy));
    CHECK_FALSE(UR_GAME_API_HAS(&api, RegisterFollowerPolicy));
    CHECK(UR_GAME_API_HAS(&api, PlayerMeleeHit));

    const UrGameApi* pNone = nullptr;
    CHECK_FALSE(UR_GAME_API_HAS(pNone, RegisterFollowerPolicy));
}
