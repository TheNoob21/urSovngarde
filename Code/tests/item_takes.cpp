#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>
#include <glm/glm.hpp>

#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/RequestItemTake.h>
#include <Messages/NotifyItemTakeLost.h>

#include <ItemTakes.h>
#include <Services/TakeUndo.h>

// Two players taking the same item at once (ItemTakes.h on the server, TakeUndo.h in the game): the first take stands, the later
// one is given back, and no count goes below zero.

using namespace TiltedPhoques;
using namespace std::chrono_literals;

namespace
{
template <class T> T ClientRoundTrip(const T& acMessage)
{
    Buffer buffer(1024);
    Buffer::Writer writer(&buffer);
    acMessage.Serialize(writer);
    Buffer::Reader reader(&buffer);
    auto pMessage = ClientMessageFactory{}.Extract(reader);
    REQUIRE(pMessage);
    REQUIRE(pMessage->GetOpcode() == T::Opcode);
    return *CastUnique<T>(std::move(pMessage));
}

template <class T> T ServerRoundTrip(const T& acMessage)
{
    Buffer buffer(1024);
    Buffer::Writer writer(&buffer);
    acMessage.Serialize(writer);
    Buffer::Reader reader(&buffer);
    auto pMessage = ServerMessageFactory{}.Extract(reader);
    REQUIRE(pMessage);
    REQUIRE(pMessage->GetOpcode() == T::Opcode);
    return *CastUnique<T>(std::move(pMessage));
}

const GameId kSword{1, 0x13989};
const GameId kArrow{1, 0x1397D};

Inventory::Entry Item(const GameId& acBaseId, const int32_t aCount, const float aHealth = 0.f)
{
    Inventory::Entry entry{};
    entry.BaseId = acBaseId;
    entry.Count = aCount;
    entry.ExtraHealth = aHealth;
    return entry;
}
} // namespace

TEST_CASE("item take messages round trip through the factories", "[itemtakes]")
{
    RequestItemTake request{};
    request.From = ItemTakeSource::kPlacedItem;
    request.Id = 0x1234567;
    request.ObjectId = GameId{2, 0x10A2F3};
    request.CellId = GameId{2, 0x133C6};
    request.WorldSpaceId = GameId{2, 0x3C};
    request.Position = glm::vec3{1024.f, -2048.f, 12.f};
    REQUIRE(ClientRoundTrip(request) == request);
    request.From = ItemTakeSource::kDroppedItem;
    REQUIRE(ClientRoundTrip(request).From == ItemTakeSource::kDroppedItem);

    for (const ItemTakeSource cFrom : {ItemTakeSource::kInventory, ItemTakeSource::kDroppedItem, ItemTakeSource::kPlacedItem})
    {
        NotifyItemTakeLost notify{};
        notify.From = cFrom;
        notify.Id = 0x1234567;
        notify.ObjectId = GameId{2, 0x10A2F3};
        notify.Item = Item(kSword, 3, 1.1f);
        notify.Item.IsQuestItem = true;
        const NotifyItemTakeLost cBack = ServerRoundTrip(notify);
        REQUIRE(cBack == notify);
        REQUIRE(cBack.From == cFrom);
    }
}

TEST_CASE("the first take of an item stands and a later one by another player is lost", "[itemtakes]")
{
    const ItemTakes::Clock::time_point start{};
    ItemTakes::Claims<uint64_t> claims;

    REQUIRE(claims.Claim(7, 1, start));
    REQUIRE_FALSE(claims.Claim(7, 2, start + 5ms));
    REQUIRE(claims.TakenByAnother(7, 2, start + 5ms));
    REQUIRE_FALSE(claims.TakenByAnother(7, 1, start + 5ms));

    // The first player taking it again, from an older save, keeps it theirs.
    REQUIRE(claims.Claim(7, 1, start + 1min));
    REQUIRE_FALSE(claims.Claim(7, 2, start + 2min));

    // Another thing is another claim.
    REQUIRE(claims.Claim(8, 2, start + 2min));
    // Player id zero is a player like any other.
    REQUIRE(claims.Claim(9, 0, start));
    REQUIRE_FALSE(claims.Claim(9, 1, start));
}

TEST_CASE("a take is someone's only for a while, and every take goes when the session ends", "[itemtakes]")
{
    const ItemTakes::Clock::time_point start{};
    ItemTakes::Claims<uint64_t> claims;

    REQUIRE(claims.Claim(7, 1, start));
    REQUIRE_FALSE(claims.Claim(7, 2, start + ItemTakes::kHeld - 1ms));
    REQUIRE(claims.Claim(7, 2, start + ItemTakes::kHeld));
    REQUIRE_FALSE(claims.Claim(7, 1, start + ItemTakes::kHeld + 1s));

    claims.Clear();
    REQUIRE(claims.Size() == 0);
    REQUIRE(claims.Claim(7, 1, start + ItemTakes::kHeld + 2s));
}

TEST_CASE("the claims stay bounded, lapsed ones first, then the oldest", "[itemtakes]")
{
    using Claims = ItemTakes::Claims<uint32_t>;
    const ItemTakes::Clock::time_point start{};
    Claims claims;

    for (uint32_t i = 0; i < Claims::kMax; ++i)
        claims.Note(i, 1, start + std::chrono::milliseconds(i));
    REQUIRE(claims.Size() == Claims::kMax);

    // None has lapsed: the oldest goes for the new one.
    claims.Note(Claims::kMax, 1, start + 10s);
    REQUIRE(claims.Size() == Claims::kMax);
    REQUIRE_FALSE(claims.TakenByAnother(0, 2, start + 10s));
    REQUIRE(claims.TakenByAnother(1, 2, start + 10s));

    // All but the newest lapsed: they all go at the next new take.
    claims.Note(Claims::kMax + 1, 1, start + ItemTakes::kHeld + 5s);
    REQUIRE(claims.Size() == 2);
}

TEST_CASE("a take the server holds enough for is given whole", "[itemtakes]")
{
    const auto cSplit = ItemTakes::Decide(5, 3, true, false);
    REQUIRE(cSplit.Given == 3);
    REQUIRE(cSplit.Refused == 0);

    REQUIRE(ItemTakes::Decide(3, 3, true, false).Refused == 0);
    REQUIRE(ItemTakes::Decide(3, 0, true, false).Given == 0);
    REQUIRE(ItemTakes::Decide(3, -2, true, false).Given == 0);
}

TEST_CASE("a race for the last of a stack gives the later player only what is left", "[itemtakes]")
{
    // Both took the one sword: the second hears none is left.
    auto split = ItemTakes::Decide(0, 1, true, false);
    REQUIRE(split.Given == 0);
    REQUIRE(split.Refused == 1);

    // Three arrows, one taken by the other player meanwhile, all three here: two are given, one goes back.
    split = ItemTakes::Decide(2, 3, true, false);
    REQUIRE(split.Given == 2);
    REQUIRE(split.Refused == 1);

    // A held count below zero, from an older copy, counts as none.
    split = ItemTakes::Decide(-4, 1, true, false);
    REQUIRE(split.Given == 0);
    REQUIRE(split.Refused == 1);
}

TEST_CASE("a shortfall nobody else made, or of a quest item, is never refused", "[itemtakes]")
{
    // The server's copy lagging the game's: the take stands.
    auto split = ItemTakes::Decide(0, 2, false, false);
    REQUIRE(split.Given == 2);
    REQUIRE(split.Refused == 0);

    // Each player's quest needs its own.
    split = ItemTakes::Decide(0, 1, true, true);
    REQUIRE(split.Given == 1);
    REQUIRE(split.Refused == 0);
}

TEST_CASE("what a container holds counts every stack of the item, whatever its extra data", "[itemtakes]")
{
    Inventory chest{};
    chest.Entries.push_back(Item(kSword, 1, 1.f));
    chest.Entries.push_back(Item(kSword, 2, 1.1f));
    chest.Entries.push_back(Item(kArrow, 20));
    chest.Entries.push_back(Item(kSword, -3));

    REQUIRE(ItemTakes::Held(chest, kSword) == 3);
    REQUIRE(ItemTakes::Held(chest, kArrow) == 20);
    REQUIRE(ItemTakes::Held(chest, GameId{1, 0x1}) == 0);
}

TEST_CASE("a take comes out of the stack it merges with first and never leaves a count below zero", "[itemtakes]")
{
    Inventory chest{};
    chest.Entries.push_back(Item(kSword, 1, 1.f));
    chest.Entries.push_back(Item(kSword, 2, 1.1f));
    chest.Entries.push_back(Item(kArrow, 20));

    // Two of the tempered swords: out of their own stack.
    ItemTakes::Remove(chest, Item(kSword, -2, 1.1f), 2);
    REQUIRE(chest.Entries.size() == 2);
    REQUIRE(ItemTakes::Held(chest, kSword) == 1);
    REQUIRE(chest.Entries[0].ExtraHealth == 1.f);

    // A sword whose health the server has a little off: still the sword it holds.
    ItemTakes::Remove(chest, Item(kSword, -1, 1.2f), 1);
    REQUIRE(ItemTakes::Held(chest, kSword) == 0);
    REQUIRE(chest.Entries.size() == 1);

    // More than is there: down to none, never a negative stack.
    ItemTakes::Remove(chest, Item(kArrow, -50), 50);
    REQUIRE(chest.Entries.empty());
    ItemTakes::Remove(chest, Item(kArrow, -1), 1);
    REQUIRE(chest.Entries.empty());
}

TEST_CASE("the old merge left a negative stack where the arbitrated take leaves none", "[itemtakes]")
{
    // Two players take the last sword: the old path counted it twice and kept a sword at -1.
    Inventory upstream{};
    upstream.Entries.push_back(Item(kSword, 1));
    upstream.AddOrRemoveEntry(Item(kSword, -1));
    upstream.AddOrRemoveEntry(Item(kSword, -1));
    REQUIRE(upstream.Entries.size() == 1);
    REQUIRE(upstream.Entries[0].Count == -1);

    const ItemTakes::Clock::time_point start{};
    ItemTakes::Claims<ItemTakes::StackKey> stacks;
    Inventory chest{};
    chest.Entries.push_back(Item(kSword, 1));
    const auto cKey = ItemTakes::Stack(42, kSword);

    const auto take = [&](const uint32_t aPlayerId, const ItemTakes::Clock::time_point aNow)
    {
        const int32_t cHeld = ItemTakes::Held(chest, kSword);
        const auto cSplit = ItemTakes::Decide(cHeld, 1, stacks.TakenByAnother(cKey, aPlayerId, aNow), false);
        ItemTakes::Remove(chest, Item(kSword, -1), cSplit.Given);
        if (ItemTakes::MarksStack(cSplit, cHeld))
            stacks.Note(cKey, aPlayerId, aNow);
        return cSplit;
    };

    REQUIRE(take(1, start).Given == 1);
    const auto cSecond = take(2, start + 30ms);
    REQUIRE(cSecond.Given == 0);
    REQUIRE(cSecond.Refused == 1);
    REQUIRE(chest.Entries.empty());

    // The same item in another chest is another stack.
    REQUIRE_FALSE(stacks.TakenByAnother(ItemTakes::Stack(43, kSword), 2, start));
}

TEST_CASE("a lost take out of a container gives back no more than this player took", "[itemtakes]")
{
    const TakeUndo::Clock::time_point start{};
    TakeUndo takes;

    takes.Note(ItemTakeSource::kInventory, 42, Item(kArrow, 2), start);
    takes.Note(ItemTakeSource::kInventory, 42, Item(kArrow, 1), start + 10ms);

    // The server refused one: one goes back, two stay taken.
    auto back = takes.Lost(ItemTakeSource::kInventory, 42, Item(kArrow, 1), start + 50ms);
    REQUIRE(back);
    REQUIRE(back->BaseId == kArrow);
    REQUIRE(back->Count == 1);

    // Refused more than is left of the takes: only what was taken.
    back = takes.Lost(ItemTakeSource::kInventory, 42, Item(kArrow, 10), start + 60ms);
    REQUIRE(back);
    REQUIRE(back->Count == 2);
    REQUIRE(takes.Size() == 0);
    REQUIRE_FALSE(takes.Lost(ItemTakeSource::kInventory, 42, Item(kArrow, 1), start + 70ms));
}

TEST_CASE("a lost take matches its source and item, and nothing else", "[itemtakes]")
{
    const TakeUndo::Clock::time_point start{};
    TakeUndo takes;

    takes.Note(ItemTakeSource::kInventory, 42, Item(kArrow, 2), start);
    REQUIRE_FALSE(takes.Lost(ItemTakeSource::kInventory, 43, Item(kArrow, 1), start));
    REQUIRE_FALSE(takes.Lost(ItemTakeSource::kInventory, 42, Item(kSword, 1), start));
    REQUIRE_FALSE(takes.Lost(ItemTakeSource::kDroppedItem, 42, Item(kArrow, 1), start));
    // A removal this game sent without a take (a hand item gone) has nothing to give back.
    REQUIRE_FALSE(takes.Lost(ItemTakeSource::kDroppedItem, 7, {}, start));
    REQUIRE(takes.Size() == 1);
}

TEST_CASE("a lost dropped or placed item is given back whole, once", "[itemtakes]")
{
    const TakeUndo::Clock::time_point start{};
    TakeUndo takes;

    takes.Note(ItemTakeSource::kDroppedItem, 7, Item(kArrow, 12), start);
    const uint64_t cPlaced = GameId{2, 0x10A2F3}.LogFormat();
    takes.Note(ItemTakeSource::kPlacedItem, cPlaced, Item(kSword, 1, 1.1f), start);

    auto back = takes.Lost(ItemTakeSource::kDroppedItem, 7, {}, start + 40ms);
    REQUIRE(back);
    REQUIRE(back->Count == 12);
    REQUIRE_FALSE(takes.Lost(ItemTakeSource::kDroppedItem, 7, {}, start + 50ms));

    back = takes.Lost(ItemTakeSource::kPlacedItem, cPlaced, {}, start + 40ms);
    REQUIRE(back);
    REQUIRE(back->BaseId == kSword);
    REQUIRE(back->Count == 1);
    REQUIRE(back->ExtraHealth == 1.1f);
    REQUIRE(takes.Size() == 0);
}

TEST_CASE("a take is kept only until the server's answer could have come, and never for a quest item", "[itemtakes]")
{
    const TakeUndo::Clock::time_point start{};
    TakeUndo takes;

    takes.Note(ItemTakeSource::kDroppedItem, 7, Item(kArrow, 1), start);
    REQUIRE_FALSE(takes.Lost(ItemTakeSource::kDroppedItem, 7, {}, start + TakeUndo::kKeptFor));

    Inventory::Entry quest = Item(kSword, 1);
    quest.IsQuestItem = true;
    takes.Note(ItemTakeSource::kPlacedItem, 9, quest, start);
    takes.Note(ItemTakeSource::kPlacedItem, 10, Item(kSword, 0), start);
    REQUIRE(takes.Size() == 0);

    takes.Note(ItemTakeSource::kDroppedItem, 8, Item(kArrow, 1), start);
    takes.Clear();
    REQUIRE_FALSE(takes.Lost(ItemTakeSource::kDroppedItem, 8, {}, start));
}

TEST_CASE("armour taken off a body is noted and given back as unworn", "[itemtakes]")
{
    const TakeUndo::Clock::time_point start{};
    TakeUndo takes;

    Inventory::Entry worn = Item(kSword, 1);
    worn.ExtraWorn = true;
    takes.Note(ItemTakeSource::kInventory, 42, worn, start);

    // The server echoes the body's entry, worn flag and all.
    const auto cBack = takes.Lost(ItemTakeSource::kInventory, 42, worn, start + 40ms);
    REQUIRE(cBack);
    REQUIRE(cBack->Count == 1);
    REQUIRE_FALSE(cBack->IsWorn());
}

TEST_CASE("a lost take gives back that very item, not another stack of the same base", "[itemtakes]")
{
    const TakeUndo::Clock::time_point start{};
    TakeUndo takes;

    takes.Note(ItemTakeSource::kInventory, 42, Item(kSword, 1, 1.f), start);
    takes.Note(ItemTakeSource::kInventory, 42, Item(kSword, 1, 1.6f), start);

    const auto cBack = takes.Lost(ItemTakeSource::kInventory, 42, Item(kSword, 2, 1.6f), start + 40ms);
    REQUIRE(cBack);
    REQUIRE(cBack->Count == 1);
    REQUIRE(cBack->ExtraHealth == 1.6f);
    REQUIRE(takes.Size() == 1);
}

TEST_CASE("a take past what the server held claims nothing, so the next lagging take is not a race", "[itemtakes]")
{
    // The server holds no arrows while both games show three: the server's copy lags, and neither take is refused.
    const ItemTakes::Clock::time_point start{};
    ItemTakes::Claims<ItemTakes::StackKey> stacks;
    Inventory chest{};
    const auto cKey = ItemTakes::Stack(42, kArrow);

    const auto take = [&](const uint32_t aPlayerId, const ItemTakes::Clock::time_point aNow)
    {
        const int32_t cHeld = ItemTakes::Held(chest, kArrow);
        const auto cSplit = ItemTakes::Decide(cHeld, 1, stacks.TakenByAnother(cKey, aPlayerId, aNow), false);
        ItemTakes::Remove(chest, Item(kArrow, -1), cSplit.Given);
        if (ItemTakes::MarksStack(cSplit, cHeld))
            stacks.Note(cKey, aPlayerId, aNow);
        return cSplit;
    };

    REQUIRE(take(1, start).Given == 1);
    REQUIRE(take(2, start + 1s).Refused == 0);
    REQUIRE(stacks.Size() == 0);
    REQUIRE(chest.Entries.empty());
}
