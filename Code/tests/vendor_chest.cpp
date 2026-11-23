// Which chest a merchant trades from, and when its game shares it (client/Services/Generic/VendorChest.h).

#include <catch2/catch.hpp>

#include <Services/Generic/VendorChest.h>

using VendorChest::Membership;
using VendorChest::PickChest;
using VendorChest::RunFrame;
using VendorChest::Trade;
using Step = Trade::Step;

TEST_CASE("A merchant trades from its one vendor faction's chest", "[vendor_chest]")
{
    SECTION("no faction, or none a vendor's")
    {
        CHECK(PickChest({}) == 0);
        CHECK(PickChest({{false, 0, 0x9CAF9}}) == 0);
    }
    SECTION("a vendor faction with a chest")
    {
        CHECK(PickChest({{false, 1, 0x1111}, {true, 0, 0x9CAF9}}) == 0x9CAF9);
    }
    SECTION("a vendor faction without a chest is passed over")
    {
        CHECK(PickChest({{true, 0, 0}, {true, 2, 0x9CAF9}}) == 0x9CAF9);
    }
    SECTION("a rank below zero is not a member")
    {
        CHECK(PickChest({{true, -1, 0x9CAF9}}) == 0);
    }
    SECTION("two factions with the same chest")
    {
        CHECK(PickChest({{true, 0, 0x9CAF9}, {true, 1, 0x9CAF9}}) == 0x9CAF9);
    }
    SECTION("two different chests: not known which the game uses")
    {
        CHECK(PickChest({{true, 0, 0x9CAF9}, {true, 0, 0x2222}}) == 0);
    }
}

TEST_CASE("A trade shares the merchant's chest once, then checks it at each barter opening", "[vendor_chest]")
{
    Trade trade;
    constexpr uint32_t cChest = 0x9CAF9;

    // No merchant spoken to.
    CHECK(trade.Update(0, false, false).What == Step::kNothing);

    // Speaking to the merchant: shared once, then waiting for the server.
    auto action = trade.Update(cChest, false, false);
    CHECK(action.What == Step::kRegister);
    CHECK(action.ChestId == cChest);
    CHECK(trade.Update(cChest, false, false).What == Step::kNothing);
    CHECK(trade.Update(cChest, false, true).What == Step::kNothing);

    // The barter menu opens: one check, however long it stays open.
    action = trade.Update(cChest, true, true);
    CHECK(action.What == Step::kCheckRestock);
    CHECK(action.ChestId == cChest);
    CHECK(trade.Update(cChest, true, true).What == Step::kNothing);

    // Closed: the chest as this game's trades left it is kept once, then nothing while the dialogue goes on.
    action = trade.Update(cChest, false, true);
    CHECK(action.What == Step::kBarterClosed);
    CHECK(action.ChestId == cChest);
    CHECK(trade.Update(cChest, false, true).What == Step::kNothing);

    // Opened again in the same conversation: checked again.
    CHECK(trade.Update(cChest, true, true).What == Step::kCheckRestock);
}

TEST_CASE("The server's answer coming after the barter menu opened is checked then", "[vendor_chest]")
{
    Trade trade;
    constexpr uint32_t cChest = 0x9CAF9;

    CHECK(trade.Update(cChest, false, false).What == Step::kRegister);
    CHECK(trade.Update(cChest, true, false).What == Step::kNothing);

    // The other games' chest arrives while the menu shows this game's: it waits.
    CHECK(trade.Defer(cChest));
    CHECK(trade.Update(cChest, true, true).What == Step::kCheckRestock);

    // At closing, this game's chest, its trades included, goes to the others.
    const auto action = trade.Update(cChest, false, true);
    CHECK(action.What == Step::kSendAll);
    CHECK(action.ChestId == cChest);
    CHECK(trade.Update(cChest, false, true).What == Step::kNothing);
}

TEST_CASE("Another game's chest is applied at once outside this player's barter menu", "[vendor_chest]")
{
    Trade trade;
    constexpr uint32_t cChest = 0x9CAF9;

    // No trade at all.
    CHECK_FALSE(trade.Defer(cChest));

    // Speaking to the merchant, menu not open.
    trade.Update(cChest, false, false);
    trade.Update(cChest, false, true);
    CHECK_FALSE(trade.Defer(cChest));

    // Bartering, but another chest changed.
    trade.Update(cChest, true, true);
    CHECK_FALSE(trade.Defer(0x2222));
    CHECK(trade.Defer(cChest));
}

TEST_CASE("A trade that ends with both menus closing together still sends its chest", "[vendor_chest]")
{
    Trade trade;
    constexpr uint32_t cChest = 0x9CAF9;

    trade.Update(cChest, false, false);
    trade.Update(cChest, true, true);
    CHECK(trade.Defer(cChest));

    // The dialogue and the barter menu closed in one frame: no merchant is spoken to now.
    auto action = trade.Update(0, false, false);
    CHECK(action.What == Step::kSendAll);
    CHECK(action.ChestId == cChest);
    CHECK(trade.Update(0, false, false).What == Step::kNothing);

    // The next merchant is shared anew.
    action = trade.Update(0x3333, false, false);
    CHECK(action.What == Step::kRegister);
    CHECK(action.ChestId == 0x3333);
}

TEST_CASE("Speaking to the same merchant again shares the chest again", "[vendor_chest]")
{
    // The server forgets a chest once its cell empties, so each conversation asks for it.
    Trade trade;
    constexpr uint32_t cChest = 0x9CAF9;

    CHECK(trade.Update(cChest, false, false).What == Step::kRegister);
    trade.Update(cChest, false, true);
    CHECK(trade.Update(0, false, false).What == Step::kNothing);
    CHECK(trade.Update(cChest, false, false).What == Step::kRegister);
}

TEST_CASE("A restock one frame before the barter menu reports open is still sent", "[vendor_chest]")
{
    // The chest's contents stand for its inventory: 10 as shared, 25 once the game restocked it.
    Trade trade;
    constexpr uint32_t cChest = 0x9CAF9;
    int chest = 10;
    int base = 10;
    const auto cRead = [&chest](uint32_t)
    {
        return chest;
    };

    CHECK(RunFrame(trade, base, cRead, cChest, false, false).What == Step::kRegister);
    for (int frame = 0; frame < 5; ++frame)
        CHECK(RunFrame(trade, base, cRead, cChest, false, true).What == Step::kNothing);

    // "Let me see what you have": the game restocks, and the barter menu reports open only on the next frame.
    chest = 25;
    CHECK(RunFrame(trade, base, cRead, cChest, false, true).What == Step::kNothing);
    CHECK(base == 10);
    const auto cAction = RunFrame(trade, base, cRead, cChest, true, true);
    CHECK(cAction.What == Step::kCheckRestock);
    CHECK(cAction.ChestId == cChest);
}

TEST_CASE("No restock is sent for a chest unchanged since the games agreed on it", "[vendor_chest]")
{
    Trade trade;
    constexpr uint32_t cChest = 0x9CAF9;
    int chest = 10;
    int base = 10;
    const auto cRead = [&chest](uint32_t)
    {
        return chest;
    };

    RunFrame(trade, base, cRead, cChest, false, false);
    RunFrame(trade, base, cRead, cChest, false, true);
    CHECK(RunFrame(trade, base, cRead, cChest, true, true).What == Step::kNothing);

    // This game's trades went out item by item: at closing they become the base, so the next opening sends nothing.
    chest = 4;
    CHECK(RunFrame(trade, base, cRead, cChest, false, true).What == Step::kNothing);
    CHECK(base == 4);
    CHECK(RunFrame(trade, base, cRead, cChest, true, true).What == Step::kNothing);
}
