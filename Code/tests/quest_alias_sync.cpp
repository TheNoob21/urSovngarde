#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <catch2/catch.hpp>

#include <string>

#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Systems/QuestAliasSync.h>
#include <Services/QuestAliasDeciders.h>
#include <QuestAliasHistory.h>

using TiltedPhoques::Buffer;
using namespace QuestAliasSync;

namespace
{
Fill Ref(uint16_t aId, uint32_t aBase)
{
    return Fill{aId, QuestAliases::kRef, GameId(0, aBase)};
}
Fill Loc(uint16_t aId, uint32_t aBase)
{
    return Fill{aId, QuestAliases::kLocation, GameId(0, aBase)};
}
} // namespace

TEST_CASE("Radiant quests are told apart by editor id", "[quest_alias_sync]")
{
    for (const char* cpId :
         {"BQ01", "BQ04", "CR01", "CR14", "CompanionsRadiantQuest", "TGRShell", "TGTQ01", "TGTQ01Handler", "TGRShell", "MGR20", "MGR20B", "MGRAppBrelyna01", "MGRShell",
          "DBRecurringStarter", "DLC1RadiantFeed"})
        CHECK(IsRadiantQuest(cpId));

    // Main, faction and the player's own quests keep their own aliases, and civil war missions run on the leader only.
    for (const char* cpId :
         {"MQ101", "C00", "TG01", "MG01", "DB01", "BQuestSomething", "CRime", "CreatureDialogueDog", "DialogueFollower", "RelationshipMarriage", "TGTQ", "CWMission03",
          "MGRitual04", "MGRitual05", ""})
        CHECK_FALSE(IsRadiantQuest(cpId));
}

TEST_CASE("One list says which quests stay each player's own", "[quest_alias_sync]")
{
    using QuestFamilies::Family;
    CHECK(QuestFamilies::Classify("RelationshipMarriageFIN") == Family::Personal);
    CHECK(QuestFamilies::Classify("WEBountyCollector") == Family::Personal);
    CHECK(QuestFamilies::Classify("DialogueFollower") == Family::Personal);
    CHECK(QuestFamilies::Classify("DGIntimidateQuest") == Family::Personal);
    CHECK(QuestFamilies::StaysPersonal("DGIntimidateQuest", true));
    CHECK(QuestFamilies::Classify("BQ01") == Family::Radiant);
    CHECK(QuestFamilies::Classify("MGR21") == Family::Radiant);
    CHECK(QuestFamilies::Classify("MQ101") == Family::Shared);
    CHECK(QuestFamilies::Classify("Favor017") == Family::Shared);

    // Personal ones never go out; radiant ones only while their targets are shared too.
    CHECK(QuestFamilies::StaysPersonal("JailQuest", true));
    CHECK(QuestFamilies::StaysPersonal("BQ01", false));
    CHECK_FALSE(QuestFamilies::StaysPersonal("BQ01", true));
    CHECK_FALSE(QuestFamilies::StaysPersonal("MQ101", false));

    // No quest is in both lists.
    for (const auto& cRadiant : QuestFamilies::kRadiant)
        CHECK(QuestFamilies::Classify(std::string(cRadiant.Text) + (cRadiant.DigitNext ? "01" : "")) == Family::Radiant);
}

TEST_CASE("Only the wanted fills that differ here are applied, locations first", "[quest_alias_sync]")
{
    const std::vector<Fill> cWanted{Ref(1, 0x100), Loc(2, 0x200), Ref(3, 0x300), Loc(4, 0x400)};

    SECTION("all the same: nothing to do")
    {
        CHECK(Differences(cWanted, cWanted).empty());
    }

    SECTION("a different reference and a different location")
    {
        const std::vector<Fill> cLocal{Ref(1, 0x101), Loc(2, 0x200), Ref(3, 0x300), Loc(4, 0x401)};
        const auto cDiff = Differences(cWanted, cLocal);
        REQUIRE(cDiff.size() == 2);
        CHECK(cDiff[0] == Loc(4, 0x400));
        CHECK(cDiff[1] == Ref(1, 0x100));
    }

    SECTION("an alias empty here, or missing here, differs")
    {
        const std::vector<Fill> cLocal{Fill{1, QuestAliases::kRef, GameId{}}, Loc(2, 0x200), Loc(4, 0x400)};
        const auto cDiff = Differences(cWanted, cLocal);
        REQUIRE(cDiff.size() == 2);
        CHECK(cDiff[0] == Ref(1, 0x100));
        CHECK(cDiff[1] == Ref(3, 0x300));
    }

    SECTION("aliases only this game has are left alone")
    {
        std::vector<Fill> local = cWanted;
        local.push_back(Ref(9, 0x999));
        CHECK(Differences(cWanted, local).empty());
    }

    SECTION("a target in another plugin differs")
    {
        std::vector<Fill> local = cWanted;
        local[0].Target = GameId(1, 0x100);
        REQUIRE(Differences(cWanted, local).size() == 1);
    }
}

TEST_CASE("Unchanged fills are sent once per quest run", "[quest_alias_sync]")
{
    const std::vector<Fill> cFirst{Ref(1, 0x100), Loc(2, 0x200)};
    std::vector<Fill> second = cFirst;
    second[0].Target.BaseId = 0x101;
    REQUIRE(Hash(cFirst) != Hash(second));
    CHECK(Hash(cFirst) == Hash(std::vector<Fill>(cFirst)));

    using Clock = SendTracker::Clock;
    const Clock::time_point cT0{};
    SendTracker tracker;
    CHECK(tracker.ShouldSend(0x10, Hash(cFirst), cT0));
    CHECK_FALSE(tracker.ShouldSend(0x10, Hash(cFirst), cT0 + std::chrono::seconds(29)));
    // Unchanged fills go out again after a while: a send the server dropped, or a member that drifted, heals.
    CHECK(tracker.ShouldSend(0x10, Hash(cFirst), cT0 + std::chrono::seconds(30)));
    // A re-pick at a later stage goes out at once.
    CHECK(tracker.ShouldSend(0x10, Hash(second), cT0 + std::chrono::seconds(31)));
    CHECK_FALSE(tracker.ShouldSend(0x10, Hash(second), cT0 + std::chrono::seconds(32)));
    // Another quest is its own; offering one only puts it on the list to check.
    tracker.Offer(0x11);
    tracker.Offer(0x11);
    CHECK(tracker.Quests().size() == 2);
    CHECK(tracker.ShouldSend(0x11, Hash(second), cT0 + std::chrono::seconds(32)));
    // Someone joined, or a save was loaded: everything goes out once more.
    tracker.Reoffer();
    CHECK(tracker.ShouldSend(0x10, Hash(second), cT0 + std::chrono::seconds(33)));
    CHECK(tracker.ShouldSend(0x11, Hash(second), cT0 + std::chrono::seconds(33)));
    CHECK_FALSE(tracker.ShouldSend(0x11, Hash(second), cT0 + std::chrono::seconds(34)));
    // A quest is offered from its first offer or send until it is forgotten: this game picked that run's targets.
    CHECK(tracker.Offered(0x10));
    CHECK(tracker.Offered(0x11));
    CHECK_FALSE(tracker.Offered(0x12));
    // A stop starts the next run afresh.
    tracker.Forget(0x10);
    CHECK_FALSE(tracker.Offered(0x10));
    CHECK(tracker.Quests().size() == 1);
    CHECK(tracker.ShouldSend(0x10, Hash(second), cT0 + std::chrono::seconds(34)));
}

TEST_CASE("Wanted fills are given up only while unapplied", "[quest_alias_sync]")
{
    using Clock = Wanted::Clock;
    const Clock::time_point cStart{};
    Wanted wanted;
    wanted.Set(0x10, {Ref(1, 0x100)}, cStart);
    wanted.Set(0x11, {Ref(1, 0x100)}, cStart);
    REQUIRE(wanted.Find(0x10)->Pending);

    CHECK(wanted.TakeExpired(cStart + std::chrono::minutes(9)).empty());
    // Applied: it stays for the rest of the run, however long.
    wanted.Find(0x11)->Pending = false;

    const auto cExpired = wanted.TakeExpired(cStart + std::chrono::minutes(10));
    REQUIRE(cExpired.size() == 1);
    CHECK(cExpired[0] == 0x10);
    // Given up, but kept: the same fills sent again are not tried again, and it is not given up on twice.
    REQUIRE(wanted.Find(0x10) != nullptr);
    CHECK(wanted.Find(0x10)->GaveUp);
    CHECK_FALSE(wanted.Find(0x10)->Pending);
    wanted.Set(0x10, {Ref(1, 0x100)}, cStart + std::chrono::minutes(11));
    CHECK_FALSE(wanted.Find(0x10)->Pending);
    wanted.Rearm(cStart + std::chrono::minutes(11));
    CHECK_FALSE(wanted.Find(0x10)->Pending);
    // The load re-armed the applied one too; applied again here, so only the given-up one is left to check.
    CHECK(wanted.Find(0x11)->Pending);
    wanted.Find(0x11)->Pending = false;
    CHECK(wanted.TakeExpired(cStart + std::chrono::minutes(30)).empty());
    // A re-pick is tried afresh.
    wanted.Set(0x10, {Ref(1, 0x200)}, cStart + std::chrono::minutes(12));
    CHECK(wanted.Find(0x10)->Pending);
    CHECK_FALSE(wanted.Find(0x10)->GaveUp);
    wanted.Forget(0x10);
    REQUIRE(wanted.Find(0x11) != nullptr);
    CHECK(wanted.TakeExpired(cStart + std::chrono::hours(5)).empty());

    // A re-pick is applied afresh, its wait starts over, and the run it belongs to is still known.
    wanted.Find(0x11)->SeenRunning = true;
    wanted.Set(0x11, {Ref(1, 0x101)}, cStart + std::chrono::minutes(14));
    const auto* pEntry = wanted.Find(0x11);
    CHECK(pEntry->Pending);
    CHECK(pEntry->SeenRunning);
    CHECK(pEntry->Fills[0] == Ref(1, 0x101));
    CHECK(wanted.TakeExpired(cStart + std::chrono::minutes(23)).empty());

    // The same fills sent again while they wait keep the wait's start: a target never found is still given up on.
    wanted.Set(0x11, {Ref(1, 0x101)}, cStart + std::chrono::minutes(20));
    CHECK(wanted.TakeExpired(cStart + std::chrono::minutes(24)).size() == 1);

    // The same fills sent again after they were applied: applied once more, to put back what drifted.
    wanted.Set(0x12, {Ref(1, 0x100)}, cStart);
    wanted.Find(0x12)->Pending = false;
    wanted.Set(0x12, {Ref(1, 0x100)}, cStart + std::chrono::minutes(30));
    CHECK(wanted.Find(0x12)->Pending);
    CHECK(wanted.TakeExpired(cStart + std::chrono::minutes(39)).empty());

    // A load applies everything again.
    wanted.Find(0x12)->Pending = false;
    wanted.Rearm(cStart + std::chrono::minutes(40));
    CHECK(wanted.Find(0x12)->Pending);
}

TEST_CASE("Received fills without a target or with an unknown kind are not used", "[quest_alias_sync]")
{
    CHECK(IsUsable(Ref(1, 0x100)));
    CHECK(IsUsable(Loc(1, 0x100)));
    CHECK_FALSE(IsUsable(Fill{1, QuestAliases::kRef, GameId{}}));
    CHECK_FALSE(IsUsable(Fill{1, 7, GameId(0, 0x100)}));
}

TEST_CASE("The first to start a quest decides its targets until it stops", "[quest_alias_sync]")
{
    const GameId cQuest(0, 0x1A2B3);
    const auto cEveryoneStays = [](uint32_t)
    {
        return true;
    };
    constexpr bool cLeader = true;
    constexpr bool cMember = false;
    QuestAliasDeciders deciders;

    SECTION("the starter's fills go out, a later starter's and others' are dropped, even the leader's")
    {
        deciders.OnStarted(1, cQuest, 7);
        deciders.OnStarted(1, cQuest, 8);
        CHECK(deciders.DeciderOf(1, cQuest) == 7u);
        CHECK(deciders.Accept(1, cQuest, 7, cMember, cEveryoneStays));
        CHECK_FALSE(deciders.Accept(1, cQuest, 8, cLeader, cEveryoneStays));
        CHECK(deciders.Accept(1, cQuest, 7, cMember, cEveryoneStays));
    }

    SECTION("with no start seen, only the leader claims it")
    {
        CHECK_FALSE(deciders.Accept(1, cQuest, 7, cMember, cEveryoneStays));
        CHECK_FALSE(deciders.DeciderOf(1, cQuest));
        CHECK(deciders.Accept(1, cQuest, 8, cLeader, cEveryoneStays));
        CHECK_FALSE(deciders.Accept(1, cQuest, 7, cMember, cEveryoneStays));
        // A start that comes later does not take it over.
        deciders.OnStarted(1, cQuest, 7);
        CHECK_FALSE(deciders.Accept(1, cQuest, 7, cMember, cEveryoneStays));
    }

    SECTION("a stop frees the next run")
    {
        deciders.OnStarted(1, cQuest, 7);
        deciders.OnStopped(1, cQuest);
        CHECK_FALSE(deciders.DeciderOf(1, cQuest));
        deciders.OnStarted(1, cQuest, 8);
        CHECK(deciders.Accept(1, cQuest, 8, cMember, cEveryoneStays));
        CHECK_FALSE(deciders.Accept(1, cQuest, 7, cLeader, cEveryoneStays));
    }

    SECTION("a decider who left the party hands it to the leader, and one who left the server is forgotten")
    {
        deciders.OnStarted(1, cQuest, 7);
        const auto cSevenLeft = [](uint32_t aId)
        {
            return aId != 7;
        };
        CHECK_FALSE(deciders.Accept(1, cQuest, 9, cMember, cSevenLeft));
        CHECK(deciders.Accept(1, cQuest, 8, cLeader, cSevenLeft));
        CHECK(deciders.DeciderOf(1, cQuest) == 8u);

        deciders.ForgetPlayer(8);
        CHECK_FALSE(deciders.DeciderOf(1, cQuest));
    }

    SECTION("parties and quests are separate")
    {
        const GameId cOther(0, 0x1A2B4);
        deciders.OnStarted(1, cQuest, 7);
        deciders.OnStarted(2, cQuest, 9);
        CHECK(deciders.Accept(1, cOther, 8, cLeader, cEveryoneStays));
        CHECK(deciders.Accept(2, cQuest, 9, cMember, cEveryoneStays));
        CHECK_FALSE(deciders.Accept(2, cQuest, 7, cMember, cEveryoneStays));
    }
}

TEST_CASE("Quest alias messages round-trip", "[quest_alias_sync]")
{
    QuestAliases aliases{};
    aliases.Quest = GameId(3, 0x801);
    aliases.Fills = {Ref(0, 0x100), Loc(5, 0x18E26), Fill{300, QuestAliases::kRef, GameId(4, 0x12345)}};

    {
        Buffer buff(1000);
        RequestQuestAliases sendMessage;
        sendMessage.Aliases = aliases;
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);
        Buffer::Reader reader(&buff);
        auto pMessage = ClientMessageFactory{}.Extract(reader);
        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == sendMessage.GetOpcode());
        REQUIRE(*CastUnique<RequestQuestAliases>(std::move(pMessage)) == sendMessage);
    }

    {
        Buffer buff(1000);
        NotifyQuestAliases sendMessage;
        sendMessage.Aliases = aliases;
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);
        Buffer::Reader reader(&buff);
        auto pMessage = ServerMessageFactory{}.Extract(reader);
        REQUIRE(pMessage);
        const auto pRecv = CastUnique<NotifyQuestAliases>(std::move(pMessage));
        REQUIRE(*pRecv == sendMessage);
        CHECK(pRecv->Aliases.Fills[2].AliasId == 300);
    }
}

TEST_CASE("A quest alias list longer than the cap is cut", "[quest_alias_sync]")
{
    // Written by hand: the struct's own writer never writes more than the cap.
    const GameId cQuest(0, 0x10);
    const size_t cSent = QuestAliases::kMaxFills + 10;
    Buffer buff(10000);
    Buffer::Writer writer(&buff);
    cQuest.Serialize(writer);
    TiltedPhoques::Serialization::WriteVarInt(writer, cSent);
    for (size_t i = 0; i < cSent; ++i)
    {
        TiltedPhoques::Serialization::WriteVarInt(writer, i);
        writer.WriteBits(QuestAliases::kRef, 8);
        GameId(0, static_cast<uint32_t>(0x100 + i)).Serialize(writer);
    }

    Buffer::Reader reader(&buff);
    QuestAliases back{};
    back.Read(reader);
    CHECK(back.Quest == cQuest);
    REQUIRE(back.Fills.size() == QuestAliases::kMaxFills);
    CHECK(back.Fills.back() == Ref(static_cast<uint16_t>(QuestAliases::kMaxFills - 1), static_cast<uint32_t>(0x100 + QuestAliases::kMaxFills - 1)));

    // The writer cuts as well.
    QuestAliases big{};
    for (uint16_t i = 0; i < cSent; ++i)
        big.Fills.push_back(Ref(i, 0x100 + i));
    Buffer buff2(10000);
    Buffer::Writer writer2(&buff2);
    big.Write(writer2);
    Buffer::Reader reader2(&buff2);
    QuestAliases back2{};
    back2.Read(reader2);
    CHECK(back2.Fills.size() == QuestAliases::kMaxFills);
}

TEST_CASE("A party retains its latest radiant targets until the quest stops", "[quest_alias_sync]")
{
    const GameId quest(0, 0xabc);
    const GameId otherQuest(0, 0xdef);
    const QuestAliases first{quest, {Ref(0, 0x100)}};
    const QuestAliases changed{quest, {Ref(0, 0x200), Loc(1, 0x300)}};

    QuestAliasHistory history;
    history.Remember(first);
    history.Remember({otherQuest, {Loc(0, 0x400)}});
    CHECK(history.GetQuests().at(quest) == first);

    history.Remember(changed);
    REQUIRE(history.GetQuests().size() == 2);
    CHECK(history.GetQuests().at(quest) == changed);

    history.Stop(quest);
    CHECK_FALSE(history.GetQuests().contains(quest));
    CHECK(history.GetQuests().contains(otherQuest));

    history.Remember(first);
    CHECK(history.IsStopped(quest));
    CHECK_FALSE(history.GetQuests().contains(quest));

    history.Start(quest);
    CHECK_FALSE(history.IsStopped(quest));
    history.Remember(first);
    CHECK(history.GetQuests().at(quest) == first);
}

TEST_CASE("Radiant target history remains bounded and current at capacity", "[quest_alias_sync]")
{
    QuestAliasHistory history;
    for (uint32_t i = 1; i <= QuestAliasHistory::kMaxQuests; ++i)
        history.Remember({GameId(0, i), {Ref(0, 0x100)}});

    const GameId quest(0, 1);
    const GameId excess(0, 0xabc);
    const QuestAliases changed{quest, {Ref(0, 0x200)}};
    history.Remember({excess, {Ref(0, 0x100)}});
    history.Remember(changed);

    CHECK(history.GetQuests().size() == QuestAliasHistory::kMaxQuests);
    CHECK_FALSE(history.GetQuests().contains(excess));
    CHECK(history.GetQuests().at(quest) == changed);
}

TEST_CASE("A stop before the first radiant fill survives picker departure until restart", "[quest_alias_sync]")
{
    const GameId quest(0, 0xabc);
    const QuestAliases aliases{quest, {Ref(0, 0x100)}};
    QuestAliasHistory history;
    QuestAliasDeciders deciders;
    deciders.OnStarted(1, quest, 10);
    deciders.OnStopped(1, quest);
    history.Stop(quest);
    deciders.ForgetPlayer(10);

    // The service checks the tombstone before allowing a leader to reclaim an empty decider.
    CHECK(history.IsStopped(quest));
    history.Remember(aliases);
    CHECK(history.GetQuests().empty());
    CHECK_FALSE(deciders.DeciderOf(1, quest));

    history.Start(quest);
    deciders.OnStarted(1, quest, 20);
    CHECK_FALSE(history.IsStopped(quest));
    CHECK(deciders.Accept(1, quest, 20, true, [](uint32_t) { return true; }));
    history.Remember(aliases);
    CHECK(history.GetQuests().at(quest) == aliases);
}

TEST_CASE("Radiant stop tombstones retain the most recently stopped quests", "[quest_alias_sync]")
{
    QuestAliasHistory history;
    for (uint32_t i = 1; i <= QuestAliasHistory::kMaxQuests; ++i)
        history.Stop(GameId(0, i));

    // A repeated stop refreshes its age without using another slot.
    history.Stop(GameId(0, 1));
    const GameId newest(0, QuestAliasHistory::kMaxQuests + 1);
    history.Stop(newest);
    CHECK(history.IsStopped(GameId(0, 1)));
    CHECK_FALSE(history.IsStopped(GameId(0, 2)));
    CHECK(history.IsStopped(newest));
    history.Remember({newest, {Ref(0, 0x100)}});
    CHECK(history.GetQuests().empty());

    history.Start(newest);
    CHECK_FALSE(history.IsStopped(newest));
    history.Stop(GameId(0, QuestAliasHistory::kMaxQuests + 2));
    CHECK(history.IsStopped(GameId(0, 3)));
}

TEST_CASE("A party remembers a bounded number of quest runs and still takes new ones", "[quest_alias_sync]")
{
    QuestAliasDeciders deciders;
    const auto cMember = [](uint32_t)
    {
        return true;
    };
    for (uint32_t quest = 1; quest <= QuestAliasDeciders::kMaxPerParty + 10; ++quest)
    {
        deciders.OnStarted(1, GameId(0, quest), 7);
        // The newest start is always remembered, and its starter decides it.
        CHECK(deciders.DeciderOf(1, GameId(0, quest)) == 7u);
        CHECK(deciders.Accept(1, GameId(0, quest), 7, false, cMember));
    }
    size_t remembered = 0;
    for (uint32_t quest = 1; quest <= QuestAliasDeciders::kMaxPerParty + 10; ++quest)
        remembered += deciders.DeciderOf(1, GameId(0, quest)).has_value();
    CHECK(remembered == QuestAliasDeciders::kMaxPerParty);
    // A forgotten quest falls back to the leader, as for one with no start seen.
    CHECK(deciders.Accept(1, GameId(0, 0x9999), 3, true, cMember));
    CHECK(deciders.DeciderOf(1, GameId(0, 0x9999)) == 3u);
}
