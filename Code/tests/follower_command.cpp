#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <catch2/catch.hpp>

#include <FollowerCommand.h>
#include <FollowerInventory.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/RequestFollowerCommand.h>
#include <Messages/NotifyFollowerCommand.h>

using TiltedPhoques::Buffer;
using namespace std::chrono_literals;

TEST_CASE("a word to a follower is passed on from another player of her player's party only", "[followercommand]")
{
    REQUIRE(FollowerCommand::MayAsk(1, true, 2, true));
    // Her own player: her game needs no word from the server.
    REQUIRE_FALSE(FollowerCommand::MayAsk(1, true, 1, true));
    // Nobody's follower, or her player gone.
    REQUIRE_FALSE(FollowerCommand::MayAsk(0, true, 2, true));
    REQUIRE_FALSE(FollowerCommand::MayAsk(1, false, 2, true));
    // Not of her player's party.
    REQUIRE_FALSE(FollowerCommand::MayAsk(1, true, 2, false));
}

TEST_CASE("only wait and follow are words, and only known answers are answers", "[followercommand]")
{
    REQUIRE(FollowerCommand::IsCommand(FollowerCommand::kWait));
    REQUIRE(FollowerCommand::IsCommand(FollowerCommand::kFollow));
    REQUIRE_FALSE(FollowerCommand::IsCommand(0));
    REQUIRE_FALSE(FollowerCommand::IsCommand(3));
    REQUIRE_FALSE(FollowerCommand::IsReply(FollowerCommand::kAsked));
    REQUIRE(FollowerCommand::IsReply(FollowerCommand::kDone));
    REQUIRE(FollowerCommand::IsReply(FollowerCommand::kGone));
    REQUIRE(FollowerCommand::IsReply(FollowerCommand::kOutOfReach));
    REQUIRE_FALSE(FollowerCommand::IsReply(6));
}

TEST_CASE("a busy follower refuses the word", "[followercommand]")
{
    REQUIRE(FollowerCommand::Check({}) == FollowerCommand::kAsked);
    FollowerCommand::State fighting{};
    fighting.InCombat = true;
    REQUIRE(FollowerCommand::Check(fighting) == FollowerCommand::kBusy);
    FollowerCommand::State down{};
    down.BleedingOut = true;
    REQUIRE(FollowerCommand::Check(down) == FollowerCommand::kBusy);
    FollowerCommand::State talking{};
    talking.InDialogue = true;
    REQUIRE(FollowerCommand::Check(talking) == FollowerCommand::kBusy);
    FollowerCommand::State riding{};
    riding.Mounted = true;
    REQUIRE(FollowerCommand::Check(riding) == FollowerCommand::kBusy);
}

TEST_CASE("another player's follower is never reported as this player's, even made a teammate for a trade, copy or not", "[followercommand]")
{
    FollowerCommand::ReportedActor own{};
    own.Loaded = true;
    REQUIRE(FollowerCommand::MayReportFollower(own));

    // On a copy (VR), or on this game's own reference (flat, no copy): the same, never reported.
    FollowerCommand::ReportedActor trading = own;
    trading.OthersFollower = true;
    REQUIRE_FALSE(FollowerCommand::MayReportFollower(trading));

    FollowerCommand::ReportedActor unloaded = own;
    unloaded.Loaded = false;
    REQUIRE_FALSE(FollowerCommand::MayReportFollower(unloaded));
    FollowerCommand::ReportedActor body = own;
    body.Dead = true;
    REQUIRE_FALSE(FollowerCommand::MayReportFollower(body));
    FollowerCommand::ReportedActor temporary = own;
    temporary.Temporary = true;
    REQUIRE_FALSE(FollowerCommand::MayReportFollower(temporary));
    FollowerCommand::ReportedActor player = own;
    player.IsThisPlayer = true;
    REQUIRE_FALSE(FollowerCommand::MayReportFollower(player));
    FollowerCommand::ReportedActor otherPlayer = own;
    otherPlayer.RemotePlayer = true;
    REQUIRE_FALSE(FollowerCommand::MayReportFollower(otherPlayer));
}

TEST_CASE("a player's words are held to a few a second", "[followercommand]")
{
    FollowerCommand::RateLimit limit{};
    const std::chrono::steady_clock::time_point cStart{100s};
    for (uint32_t i = 0; i < FollowerCommand::RateLimit::kPerSecond; ++i)
        REQUIRE(limit.Allow(cStart + std::chrono::milliseconds(i)));
    REQUIRE_FALSE(limit.Allow(cStart + 500ms));
    REQUIRE(limit.Allow(cStart + 1s));
}

TEST_CASE("a player's follower reports are held to a few a second, more than words", "[followercommand]")
{
    FollowerCommand::ClaimRateLimit limit{};
    static_assert(FollowerCommand::ClaimRateLimit::kPerSecond > FollowerCommand::RateLimit::kPerSecond);
    const std::chrono::steady_clock::time_point cStart{100s};
    for (uint32_t i = 0; i < FollowerCommand::ClaimRateLimit::kPerSecond; ++i)
        REQUIRE(limit.Allow(cStart + std::chrono::milliseconds(i)));
    REQUIRE_FALSE(limit.Allow(cStart + 500ms));
    REQUIRE_FALSE(limit.Allow(cStart + 999ms));
    REQUIRE(limit.Allow(cStart + 1s));
}

TEST_CASE("only the exact open remote follower trade can add items without an old container", "[followercommand][inventory]")
{
    FollowerCommand::TradeInventory trade{};
    trade.RemoteFollower = trade.Loaded = trade.InParty = trade.InReach = trade.MenuOpen = trade.MenuTarget = true;
    REQUIRE(FollowerCommand::MayAddTradeItem(trade, 7, false));
    // No exception for zero/removal, or a different native transfer source.
    REQUIRE_FALSE(FollowerCommand::MayAddTradeItem(trade, 0, false));
    REQUIRE_FALSE(FollowerCommand::MayAddTradeItem(trade, -3, false));
    REQUIRE_FALSE(FollowerCommand::MayAddTradeItem(trade, 7, true));
    // A menu alone, another menu's target, a stale follower or a player is no authority.
    for (auto field :
         {&FollowerCommand::TradeInventory::RemoteFollower, &FollowerCommand::TradeInventory::Loaded, &FollowerCommand::TradeInventory::InParty,
          &FollowerCommand::TradeInventory::InReach, &FollowerCommand::TradeInventory::MenuOpen, &FollowerCommand::TradeInventory::MenuTarget})
    {
        auto absent = trade;
        absent.*field = false;
        REQUIRE_FALSE(FollowerCommand::MayAddTradeItem(absent, 7, false));
    }
    trade.Dead = true;
    REQUIRE_FALSE(FollowerCommand::MayAddTradeItem(trade, 7, false));
}

TEST_CASE("only the actual local native follower owner accepts foreign count changes", "[followercommand][inventory]")
{
    FollowerCommand::InventoryOwner owner{true, true, false, false, true};
    REQUIRE(FollowerCommand::MayApplyOwnerInventory(owner));
    auto changed = owner;
    changed.CurrentLocalOwner = false; // a copy, unknown actor, or a stale/nonmatching epoch
    REQUIRE_FALSE(FollowerCommand::MayApplyOwnerInventory(changed));
    changed = owner;
    changed.LocalActor = false;
    REQUIRE_FALSE(FollowerCommand::MayApplyOwnerInventory(changed));
    changed = owner;
    changed.Player = true;
    REQUIRE_FALSE(FollowerCommand::MayApplyOwnerInventory(changed));
    changed = owner;
    changed.OthersFollower = true;
    REQUIRE_FALSE(FollowerCommand::MayApplyOwnerInventory(changed));
    changed = owner;
    changed.NativeFollower = false;
    REQUIRE_FALSE(FollowerCommand::MayApplyOwnerInventory(changed));
}

TEST_CASE("owner inventory retains every stack field while ignoring foreign worn flags", "[followercommand][inventory]")
{
    Inventory::Entry received{};
    received.BaseId = GameId{3, 0x123};
    received.ExtraCharge = 45.f;
    received.ExtraEnchantId = GameId{4, 0x456};
    received.ExtraEnchantCharge = 321;
    received.EnchantData.IsWeapon = true;
    received.EnchantData.Effects.push_back({2.f, 3, 4, 5.f, GameId{1, 0x789}});
    received.ExtraHealth = 1.25f;
    received.ExtraPoisonId = GameId{5, 0xABC};
    received.ExtraPoisonCount = 2;
    received.ExtraSoulLevel = 4;
    received.ExtraEnchantRemoveUnequip = true;
    received.ExtraWorn = received.ExtraWornLeft = received.IsQuestItem = true;
    for (const int32_t count : {7, -3})
    {
        received.Count = count;
        const Inventory::Entry original = received;
        const Inventory::Entry applied = FollowerOwnerItem(received);
        auto expected = received;
        expected.ExtraWorn = expected.ExtraWornLeft = false;
        REQUIRE(applied == expected);
        REQUIRE(applied.EnchantData.IsWeapon == received.EnchantData.IsWeapon);
        REQUIRE(applied.EnchantData.Effects.size() == 1);
        REQUIRE(applied.EnchantData.Effects.front().Magnitude == 2.f);
        REQUIRE(applied.EnchantData.Effects.front().Area == 3);
        REQUIRE(applied.EnchantData.Effects.front().Duration == 4);
        REQUIRE(applied.EnchantData.Effects.front().RawCost == 5.f);
        REQUIRE(applied.EnchantData.Effects.front().EffectId == received.EnchantData.Effects.front().EffectId);
        REQUIRE(received == original);
        REQUIRE(received.ExtraWorn);
        REQUIRE(received.ExtraWornLeft);
        REQUIRE(applied.Count == count);
    }
}

TEST_CASE("the follower messages carry the word and the answer", "[followercommand][encoding.packets]")
{
    Buffer buff(1000);

    {
        RequestFollowerCommand request;
        request.ServerId = 0x1234;
        request.Command = FollowerCommand::kFollow;
        request.Reply = FollowerCommand::kBusy;
        request.ReplyToPlayerId = 300;

        Buffer::Writer writer(&buff);
        request.Serialize(writer);
        Buffer::Reader reader(&buff);

        const ClientMessageFactory factory;
        auto pMessage = factory.Extract(reader);
        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == kRequestFollowerCommand);
        auto pRequest = TiltedPhoques::CastUnique<RequestFollowerCommand>(std::move(pMessage));
        REQUIRE(*pRequest == request);
    }

    {
        NotifyFollowerCommand notify;
        notify.ServerId = 0x1234;
        notify.Command = FollowerCommand::kWait;
        notify.Reply = FollowerCommand::kAsked;
        notify.FromPlayerId = 7;

        Buffer::Writer writer(&buff);
        notify.Serialize(writer);
        Buffer::Reader reader(&buff);

        const ServerMessageFactory factory;
        auto pMessage = factory.Extract(reader);
        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == kNotifyFollowerCommand);
        auto pNotify = TiltedPhoques::CastUnique<NotifyFollowerCommand>(std::move(pMessage));
        REQUIRE(*pNotify == notify);
        REQUIRE(pNotify->FromPlayerId == 7);
    }
}
