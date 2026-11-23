#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

// The encoding structs name TiltedCore's types without the namespace, as Message.h does for the message tests.
using namespace TiltedPhoques;

#include <glm/vec3.hpp>

#include <ContainerLedger.h>

namespace
{
const GameId cChest(0, 0x34EE1);

Inventory Gold(int32_t aCount)
{
    Inventory content{};
    Inventory::Entry gold{};
    gold.BaseId = GameId(0, 0xF);
    gold.Count = aCount;
    content.Entries.push_back(gold);
    return content;
}

LockData Locked(uint8_t aLevel)
{
    LockData lock{};
    lock.IsLocked = true;
    lock.LockLevel = aLevel;
    return lock;
}

// A place in Tamriel's worldspace, by grid cell.
ContainerLedger::Place Outdoors(int32_t aX, int32_t aY)
{
    return {GameId(0, 0x9DB5), GameId(0, 0x3C), GridCellCoords(aX, aY)};
}

ContainerLedger::Place Indoors(uint32_t aCellId)
{
    return {GameId(0, aCellId), {}, {}};
}
} // namespace

TEST_CASE("a remembered container is recalled with its content, lock and place", "[chest-ledger]")
{
    ContainerLedger ledger;
    ledger.Remember(cChest, Gold(5), Locked(25), Outdoors(5, -12));

    const ContainerLedger::Entry* pEntry = ledger.Recall(cChest);
    REQUIRE(pEntry != nullptr);
    REQUIRE(pEntry->Content == Gold(5));
    REQUIRE(pEntry->Lock == Locked(25));
    REQUIRE(pEntry->Where.Cell == GameId(0, 0x9DB5));
    REQUIRE(pEntry->Where.WorldSpaceId == GameId(0, 0x3C));
    REQUIRE(pEntry->Where.CenterCoords == GridCellCoords(5, -12));

    SECTION("and one never remembered is not")
    {
        REQUIRE(ledger.Recall(GameId(0, 0x34EE2)) == nullptr);
        REQUIRE(ledger.Recall(GameId(1, 0x34EE1)) == nullptr);
    }
}

TEST_CASE("remembered twice, a container keeps its latest content", "[chest-ledger]")
{
    ContainerLedger ledger;
    ledger.Remember(cChest, Gold(5), Locked(25), Outdoors(5, -12));
    ledger.Remember(cChest, Gold(2), LockData{}, Outdoors(5, -12));

    REQUIRE(ledger.Size() == 1);
    REQUIRE(ledger.Recall(cChest)->Content == Gold(2));
    REQUIRE_FALSE(ledger.Recall(cChest)->Lock.IsLocked);
}

TEST_CASE("a recall leaves the container remembered", "[chest-ledger]")
{
    ContainerLedger ledger;
    ledger.Remember(cChest, Gold(5), LockData{}, Outdoors(5, -12));

    REQUIRE(ledger.Recall(cChest) != nullptr);
    REQUIRE(ledger.Recall(cChest) != nullptr);
    REQUIRE(ledger.Recall(cChest)->Content == Gold(5));
    REQUIRE(ledger.Size() == 1);

    // Until the server forgets the container again and remembers it anew.
    ledger.Remember(cChest, Gold(1), LockData{}, Outdoors(5, -12));
    REQUIRE(ledger.Recall(cChest)->Content == Gold(1));
}

TEST_CASE("past 4096 containers the one remembered longest ago goes first", "[chest-ledger]")
{
    constexpr uint32_t cMax = static_cast<uint32_t>(ContainerLedger::kMax);
    ContainerLedger ledger;
    for (uint32_t i = 1; i <= cMax; ++i)
        ledger.Remember(GameId(0, i), Gold(1), LockData{}, Outdoors(0, 0));
    REQUIRE(ledger.Size() == ContainerLedger::kMax);

    SECTION("the 4097th takes the oldest one's place")
    {
        ledger.Remember(GameId(0, cMax + 1), Gold(3), LockData{}, Outdoors(0, 0));

        REQUIRE(ledger.Size() == ContainerLedger::kMax);
        REQUIRE(ledger.Recall(GameId(0, 1)) == nullptr);
        REQUIRE(ledger.Recall(GameId(0, 2)) != nullptr);
        REQUIRE(ledger.Recall(GameId(0, cMax + 1))->Content == Gold(3));
    }

    SECTION("one remembered again counts as remembered now")
    {
        ledger.Remember(GameId(0, 1), Gold(2), LockData{}, Outdoors(0, 0));
        ledger.Remember(GameId(0, cMax + 1), Gold(3), LockData{}, Outdoors(0, 0));

        REQUIRE(ledger.Size() == ContainerLedger::kMax);
        REQUIRE(ledger.Recall(GameId(0, 1))->Content == Gold(2));
        REQUIRE(ledger.Recall(GameId(0, 2)) == nullptr);
    }
}

TEST_CASE("a cleared ledger recalls nothing", "[chest-ledger]")
{
    ContainerLedger ledger;
    ledger.Remember(cChest, Gold(5), LockData{}, Outdoors(5, -12));
    ledger.Remember(GameId(0, 0x34EE2), Gold(1), LockData{}, Indoors(0x1000));

    ledger.Clear();

    REQUIRE(ledger.Size() == 0);
    REQUIRE(ledger.Recall(cChest) == nullptr);
    REQUIRE(ledger.Recall(GameId(0, 0x34EE2)) == nullptr);
}

TEST_CASE("an emptied container is remembered as empty", "[chest-ledger]")
{
    // A player looted everything: the next game in must get the empty chest, not its own full copy.
    ContainerLedger ledger;
    ledger.Remember(cChest, Inventory{}, LockData{}, Outdoors(5, -12));

    const ContainerLedger::Entry* pEntry = ledger.Recall(cChest);
    REQUIRE(pEntry != nullptr);
    REQUIRE(pEntry->Content.Entries.empty());
}

TEST_CASE("quest items in a remembered container come back as they were", "[chest-ledger]")
{
    // The game that takes the content keeps its own quest items (SetInventoryRetainingQuestItems); the ledger changes nothing.
    Inventory content = Gold(5);
    Inventory::Entry questItem{};
    questItem.BaseId = GameId(0, 0x2BCD0);
    questItem.Count = 1;
    questItem.IsQuestItem = true;
    content.Entries.push_back(questItem);

    ContainerLedger ledger;
    ledger.Remember(cChest, content, LockData{}, Outdoors(5, -12));

    const ContainerLedger::Entry* pEntry = ledger.Recall(cChest);
    REQUIRE(pEntry != nullptr);
    REQUIRE(pEntry->Content == content);
    REQUIRE(pEntry->Content.ContainsQuestItems());
    REQUIRE(pEntry->Content.Entries[1].IsQuestItem);
}

TEST_CASE("a ledger skips nothing by itself: the vendor skip is the caller's rule", "[chest-ledger]")
{
    // ObjectService asks ShouldRemember first; the ledger keeps whatever it is given.
    ContainerLedger ledger;
    ledger.Remember(GameId(0, 0x1A6C0), Gold(500), LockData{}, Indoors(0x1000));
    REQUIRE(ledger.Recall(GameId(0, 0x1A6C0)) != nullptr);

    REQUIRE_FALSE(ContainerLedger::ShouldRemember(true, false, true));
}

TEST_CASE("only a container that is no merchant's chest and held something is remembered", "[chest-ledger]")
{
    // A chest that held something, emptied or not.
    REQUIRE(ContainerLedger::ShouldRemember(false, false, true));
    // A merchant's chest, by the game's vendor data, or sent whole as only a merchant's chest is: the game restocks it.
    REQUIRE_FALSE(ContainerLedger::ShouldRemember(true, false, true));
    REQUIRE_FALSE(ContainerLedger::ShouldRemember(false, true, true));
    REQUIRE_FALSE(ContainerLedger::ShouldRemember(true, true, true));
    // Never held anything, as a door: its lock is the game's own.
    REQUIRE_FALSE(ContainerLedger::ShouldRemember(false, false, false));
}

TEST_CASE("an object goes once no player has its place in range", "[chest-ledger]")
{
    // A chest of the next cell over: grid [5,-12], announced by a player whose loaded cells are centred on [5,-11].
    const ContainerLedger::Place cNeighbour = Outdoors(5, -12);

    SECTION("it stays while a player has it in range")
    {
        REQUIRE_FALSE(ContainerLedger::InNobodysRange(cNeighbour, {Outdoors(5, -11)}));
        // Two cells out is still in the 5x5 a game has loaded.
        REQUIRE_FALSE(ContainerLedger::InNobodysRange(cNeighbour, {Outdoors(7, -10)}));
        // One player in range is enough.
        REQUIRE_FALSE(ContainerLedger::InNobodysRange(cNeighbour, {Indoors(0x1000), Outdoors(4, -13)}));
    }

    SECTION("it goes once none has")
    {
        REQUIRE(ContainerLedger::InNobodysRange(cNeighbour, {Outdoors(5, -15)}));
        REQUIRE(ContainerLedger::InNobodysRange(cNeighbour, {Outdoors(8, -12)}));
        // The same grid of another worldspace, or indoors.
        REQUIRE(ContainerLedger::InNobodysRange(cNeighbour, {ContainerLedger::Place{GameId(0, 0x9DB5), GameId(2, 0x1), GridCellCoords(5, -12)}}));
        REQUIRE(ContainerLedger::InNobodysRange(cNeighbour, {Indoors(0x1000)}));
        // A player leaving the server has no place any more.
        REQUIRE(ContainerLedger::InNobodysRange(cNeighbour, {ContainerLedger::Place{}}));
        REQUIRE(ContainerLedger::InNobodysRange(cNeighbour, {}));
    }

    SECTION("indoors only a player in the same cell has it in range")
    {
        const ContainerLedger::Place cChestInside = Indoors(0x1000);
        REQUIRE_FALSE(ContainerLedger::InNobodysRange(cChestInside, {Indoors(0x1000)}));
        REQUIRE(ContainerLedger::InNobodysRange(cChestInside, {Indoors(0x1001)}));
        REQUIRE(ContainerLedger::InNobodysRange(cChestInside, {Outdoors(0, 0)}));
    }
}

TEST_CASE("a container forgotten is no longer recalled, and the rest stay", "[chest-ledger]")
{
    ContainerLedger ledger;
    const GameId cOther(0, 0x34EE2);
    ledger.Remember(cChest, Gold(5), {}, Outdoors(5, -12));
    ledger.Remember(cOther, Gold(2), {}, Outdoors(5, -12));

    ledger.Forget(cChest);

    REQUIRE(ledger.Recall(cChest) == nullptr);
    REQUIRE(ledger.Recall(cOther) != nullptr);
    REQUIRE(ledger.Size() == 1);

    // Forgetting one that is not there changes nothing.
    ledger.Forget(cChest);
    REQUIRE(ledger.Size() == 1);
}
