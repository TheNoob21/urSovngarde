#include <catch2/catch.hpp>

#include <Systems/QuestRestart.h>

using namespace QuestRestart;

TEST_CASE("a stage the party sends for a quest stopped here", "[quests]")
{
    SECTION("is not taken when the quest ran here and stopped itself, as it would run again from its first stage")
    {
        // A misc quest done here at stage 20, a moment before the party's stage 200 of the same run comes.
        CHECK_FALSE(TakesStage({true, true, false}));
    }

    SECTION("is taken for a run the party started, while the game leaves that start for a later frame")
    {
        CHECK(TakesStage({true, true, true}));
    }

    SECTION("is taken when the quest never ran here, which starts it")
    {
        CHECK(TakesStage({true, false, false}));
    }

    SECTION("is taken while the quest runs or is starting here")
    {
        CHECK(TakesStage({false, true, false}));
        CHECK(TakesStage({false, false, false}));
    }
}
