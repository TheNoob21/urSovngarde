// Which item kinds a copy holds that its owner does not (client/LootKinds.h).

#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <catch2/catch.hpp>

#include <LootKinds.h>

namespace
{
constexpr uint32_t cLevelledList = 0x9AF0A;

Inventory::Entry Item(uint32_t aBaseId, int32_t aCount, bool aWorn = false, bool aQuest = false)
{
    Inventory::Entry entry{};
    entry.BaseId = GameId{0, aBaseId};
    entry.Count = aCount;
    entry.ExtraWorn = aWorn;
    entry.IsQuestItem = aQuest;
    return entry;
}

Inventory Holding(std::initializer_list<Inventory::Entry> aEntries)
{
    Inventory inventory{};
    for (const auto& entry : aEntries)
        inventory.Entries.push_back(entry);
    return inventory;
}

bool IsItem(const GameId& acId)
{
    return acId.BaseId != cLevelledList;
}

Vector<GameId> Ids(std::initializer_list<uint32_t> aIds)
{
    Vector<GameId> ids;
    for (uint32_t id : aIds)
        ids.push_back(GameId{0, id});
    return ids;
}
} // namespace

TEST_CASE("The same kinds in another order, count and wear are no difference", "[lootkinds]")
{
    const Inventory cOwner = Holding({Item(0x12EB7, 1, true), Item(0xF, 25), Item(0x1397E, 3)});
    const Inventory cCopy = Holding({Item(0x1397E, 1), Item(0xF, 40), Item(0x12EB7, 1, false)});
    CHECK(LootKinds::Compare(cOwner, cCopy, IsItem).IsEmpty());
}

TEST_CASE("A copy's own gear on top of its owner's is extra", "[lootkinds]")
{
    const Inventory cOwner = Holding({Item(0x12EB7, 1, true), Item(0xF, 25)});
    const Inventory cCopy = Holding({Item(0x12EB7, 1, true), Item(0xF, 25), Item(0x1B3A3, 1, true), Item(0x13950, 1)});
    const auto cDifference = LootKinds::Compare(cOwner, cCopy, IsItem);
    CHECK(cDifference.Extra == Ids({0x13950, 0x1B3A3}));
    CHECK(cDifference.Missing.empty());
}

TEST_CASE("An owner's item the copy lacks is missing", "[lootkinds]")
{
    const Inventory cOwner = Holding({Item(0x12EB7, 1, true), Item(0x3EADD, 2)});
    const Inventory cCopy = Holding({Item(0x12EB7, 1, true)});
    const auto cDifference = LootKinds::Compare(cOwner, cCopy, IsItem);
    CHECK(cDifference.Extra.empty());
    CHECK(cDifference.Missing == Ids({0x3EADD}));
}

TEST_CASE("A kind whose entries add up to none is not held", "[lootkinds]")
{
    // The base container gives one, the changes take it back.
    const Inventory cOwner = Holding({Item(0xF, 10)});
    const Inventory cCopy = Holding({Item(0xF, 10), Item(0x1397E, 1), Item(0x1397E, -1), Item(0x13950, 0)});
    CHECK(LootKinds::Compare(cOwner, cCopy, IsItem).IsEmpty());
}

TEST_CASE("Levelled lists are no items", "[lootkinds]")
{
    const Inventory cOwner = Holding({Item(0xF, 10), Item(cLevelledList, 1)});
    const Inventory cCopy = Holding({Item(0xF, 10)});
    CHECK(LootKinds::Compare(cOwner, cCopy, IsItem).IsEmpty());
    CHECK(LootKinds::Compare(cCopy, cOwner, IsItem).IsEmpty());
}

TEST_CASE("The copy's own quest items are kept and are no difference", "[lootkinds]")
{
    const Inventory cOwner = Holding({Item(0xF, 10), Item(0x2A, 1, false, true)});
    const Inventory cCopy = Holding({Item(0xF, 10), Item(0x2A, 1, false, true), Item(0x2B, 1, false, true)});
    CHECK(LootKinds::Compare(cOwner, cCopy, IsItem).IsEmpty());

    // An owner's quest item the copy holds as an ordinary item is the same kind.
    const Inventory cPlain = Holding({Item(0xF, 10), Item(0x2A, 1)});
    CHECK(LootKinds::Compare(cOwner, cPlain, IsItem).IsEmpty());
}

TEST_CASE("Kinds are told apart by plugin too", "[lootkinds]")
{
    Inventory owner = Holding({Item(0x800, 1)});
    Inventory copy = Holding({Item(0x800, 1)});
    copy.Entries[0].BaseId.ModId = 2;
    const auto cDifference = LootKinds::Compare(owner, copy, IsItem);
    CHECK(cDifference.Extra.size() == 1);
    CHECK(cDifference.Missing.size() == 1);
}
