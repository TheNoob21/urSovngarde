#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

// The encoding structs name TiltedCore's types without the namespace, as Message.h does for the message tests.
using namespace TiltedPhoques;

#include <QuestHistory.h>

namespace
{
QuestStep Step(uint32_t aBaseId, uint16_t aStage, uint8_t aStatus)
{
    return {GameId(0, aBaseId), aStage, aStatus, 1};
}
} // namespace

TEST_CASE("a party keeps the quest steps it was sent for a player who joins later", "[quests]")
{
    QuestHistory history;

    SECTION("in the order they came")
    {
        history.Record(Step(0x3372B, 0, QuestHistory::kStarted));
        history.Record(Step(0x3372B, 10, QuestHistory::kStageUpdate));
        history.Record(Step(0x46EF2, 5, QuestHistory::kStageUpdate));
        history.Record(Step(0x3372B, 20, QuestHistory::kStageUpdate));

        const auto& steps = history.GetSteps();
        REQUIRE(steps.size() == 4);
        REQUIRE(steps[0].Status == QuestHistory::kStarted);
        REQUIRE(steps[1].Stage == 10);
        REQUIRE(steps[2].Id == GameId(0, 0x46EF2));
        REQUIRE(steps[3].Stage == 20);
    }

    SECTION("each stage once, as a stage repeated for each hold of the war")
    {
        history.Record(Step(0x19E53, 4, QuestHistory::kStageUpdate));
        history.Record(Step(0x19E53, 4, QuestHistory::kStageUpdate));
        history.Record(Step(0x19E53, 9000, QuestHistory::kStageUpdate));

        REQUIRE(history.GetSteps().size() == 2);
    }

    SECTION("a quest started again keeps only its new run, other quests stay")
    {
        history.Record(Step(0x3372B, 0, QuestHistory::kStarted));
        history.Record(Step(0x3372B, 10, QuestHistory::kStageUpdate));
        history.Record(Step(0x46EF2, 5, QuestHistory::kStageUpdate));
        history.Record(Step(0x3372B, 200, QuestHistory::kStopped));
        history.Record(Step(0x3372B, 0, QuestHistory::kStarted));
        history.Record(Step(0x3372B, 10, QuestHistory::kStageUpdate));

        const auto& steps = history.GetSteps();
        REQUIRE(steps.size() == 3);
        REQUIRE(steps[0].Id == GameId(0, 0x46EF2));
        REQUIRE(steps[1].Status == QuestHistory::kStarted);
        REQUIRE(steps[2].Stage == 10);
    }

    SECTION("a stop is kept after the stages, so the joining game runs them and then stops it")
    {
        history.Record(Step(0x3372B, 10, QuestHistory::kStageUpdate));
        history.Record(Step(0x3372B, 200, QuestHistory::kStopped));

        REQUIRE(history.GetSteps().size() == 2);
        REQUIRE(history.GetSteps().back().Status == QuestHistory::kStopped);
    }

    SECTION("the oldest go first once it is full")
    {
        for (uint32_t i = 0; i < QuestHistory::kMax + 5; ++i)
            history.Record(Step(0x100000 + i, 10, QuestHistory::kStageUpdate));

        const auto& steps = history.GetSteps();
        REQUIRE(steps.size() == QuestHistory::kMax);
        REQUIRE(steps.front().Id == GameId(0, 0x100005));
        REQUIRE(steps.back().Id == GameId(0, 0x100000 + QuestHistory::kMax + 4));
    }
}

TEST_CASE("a party game is sent the party's quest steps again after it loads a save, the leader's too", "[quests]")
{
    SECTION("on joining, as before: a member, with the switch on and something kept")
    {
        CHECK(QuestHistory::MaySendCatchUp(false, true, true));
        CHECK_FALSE(QuestHistory::MaySendCatchUp(true, true, true));
        CHECK_FALSE(QuestHistory::MaySendCatchUp(false, false, true));
        CHECK_FALSE(QuestHistory::MaySendCatchUp(false, true, false));
    }

    SECTION("a first ask after a load is answered, whoever asks: the party's progress wins over an older save")
    {
        CHECK(QuestHistory::MayAnswerAsk(true, true, -1));
    }

    SECTION("an ask within 1 s of the last answered one is not")
    {
        CHECK_FALSE(QuestHistory::MayAnswerAsk(true, true, 0));
        CHECK_FALSE(QuestHistory::MayAnswerAsk(true, true, QuestHistory::kAskAgainAfterMs - 1));
        CHECK(QuestHistory::MayAnswerAsk(true, true, QuestHistory::kAskAgainAfterMs));
    }

    SECTION("the switch and an empty list still decide an ask")
    {
        CHECK_FALSE(QuestHistory::MayAnswerAsk(false, true, 60000));
        CHECK_FALSE(QuestHistory::MayAnswerAsk(true, false, 60000));
    }
}

TEST_CASE("only a quest step new to the party lets a member's switches through", "[quests]")
{
    QuestHistory history;
    history.Record(Step(0x3372B, 0, QuestHistory::kStarted));
    history.Record(Step(0x3372B, 10, QuestHistory::kStageUpdate));
    history.Record(Step(0x3372B, 50, QuestHistory::kStageUpdate));

    SECTION("a stage the party has not had in this run is new")
    {
        CHECK(history.IsNew(Step(0x3372B, 60, QuestHistory::kStageUpdate)));
    }

    SECTION("a member behind the party reaching a stage it already had is not, so it cannot take the world back")
    {
        CHECK_FALSE(history.IsNew(Step(0x3372B, 10, QuestHistory::kStageUpdate)));
        CHECK_FALSE(history.IsNew(Step(0x3372B, 50, QuestHistory::kStageUpdate)));
    }

    SECTION("a member's own quest the party never had is new, start, stages and stop")
    {
        CHECK(history.IsNew(Step(0x46EF2, 0, QuestHistory::kStarted)));
        CHECK(history.IsNew(Step(0x46EF2, 5, QuestHistory::kStageUpdate)));
        CHECK(history.IsNew(Step(0x46EF2, 5, QuestHistory::kStopped)));
    }

    SECTION("a start of a quest running for the party is not; its stop is, once")
    {
        CHECK_FALSE(history.IsNew(Step(0x3372B, 0, QuestHistory::kStarted)));
        CHECK(history.IsNew(Step(0x3372B, 50, QuestHistory::kStopped)));
        history.Record(Step(0x3372B, 50, QuestHistory::kStopped));
        CHECK_FALSE(history.IsNew(Step(0x3372B, 50, QuestHistory::kStopped)));
        CHECK(history.IsNew(Step(0x3372B, 0, QuestHistory::kStarted)));
    }

    SECTION("a quest started again runs its stages again")
    {
        history.Record(Step(0x3372B, 50, QuestHistory::kStopped));
        history.Record(Step(0x3372B, 0, QuestHistory::kStarted));
        CHECK(history.IsNew(Step(0x3372B, 10, QuestHistory::kStageUpdate)));
    }
}

TEST_CASE("a member's step opens its switch window only when it can be told apart from a member behind the party", "[quests]")
{
    constexpr auto kStage = QuestHistory::kStageUpdate;
    constexpr auto kStart = QuestHistory::kStarted;
    constexpr auto kStop = QuestHistory::kStopped;

    CHECK(QuestHistory::OpensSwitchWindow(false, true, true, kStage, true));
    CHECK(QuestHistory::OpensSwitchWindow(false, true, false, kStart, true));
    CHECK(QuestHistory::OpensSwitchWindow(false, true, true, kStop, true));

    SECTION("never the leader's, whose switches are always taken")
    {
        CHECK_FALSE(QuestHistory::OpensSwitchWindow(true, true, true, kStage, true));
    }
    SECTION("not with the party's steps not kept, as nothing tells a member behind the party then")
    {
        CHECK_FALSE(QuestHistory::OpensSwitchWindow(false, false, true, kStage, true));
    }
    SECTION("not for a step the party already had")
    {
        CHECK_FALSE(QuestHistory::OpensSwitchWindow(false, true, true, kStage, false));
    }
    SECTION("not for a stage of a quest that player never started")
    {
        CHECK_FALSE(QuestHistory::OpensSwitchWindow(false, true, false, kStage, true));
    }
}
