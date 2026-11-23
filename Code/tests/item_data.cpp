#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <Messages/RequestItemDataChange.h>
#include <Messages/NotifyItemDataChange.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Structs/ItemDataChange.h>
#include <DecodeGuard.h>
#include <cmath>
#include <limits>

#include <VRAddressOverrides.h>

using namespace TiltedPhoques;

// Items a player made or changed (rc7.12 itemdata): a made potion or poison travels as what it does, a name with its item,
// and a change where an item lies (temper, charge, poison, soul) as an ItemDataChange.

namespace
{
constexpr uint32_t kRunTime = ItemDataChanges::kRunTimeMod;

CreatedAlchemy FortifySmithing()
{
    CreatedAlchemy potion{};
    potion.Name = "Potion of Fortify Smithing";
    potion.Effects.push_back({12.5f, 0, 30, 41.25f, GameId(0, 0x7A102)});
    potion.Effects.push_back({8.f, 0, 60, 20.f, GameId(0, 0x3EB15)});
    return potion;
}

CreatedAlchemy Paralysis()
{
    CreatedAlchemy poison{};
    poison.Name = "Poison of Paralysis";
    poison.IsPoison = true;
    poison.Effects.push_back({1.f, 0, 6, 120.f, GameId(0, 0x73F30)});
    return poison;
}

Inventory::Entry MadePotion(const int32_t aCount)
{
    Inventory::Entry entry{};
    entry.Data.Base = FortifySmithing();
    entry.BaseId = GameId(kRunTime, entry.Data.Base.Key());
    entry.Count = aCount;
    return entry;
}

Inventory::Entry Dagger(const int32_t aCount)
{
    Inventory::Entry entry{};
    entry.BaseId = GameId(0, 0x1397E);
    entry.Count = aCount;
    return entry;
}

Inventory::Entry Poisoned(Inventory::Entry aEntry, const uint32_t aDoses)
{
    aEntry.Data.Poison = Paralysis();
    aEntry.ExtraPoisonId = GameId(kRunTime, aEntry.Data.Poison.Key());
    aEntry.ExtraPoisonCount = aDoses;
    return aEntry;
}

template <class T> T RoundTrip(const T& acSent)
{
    Buffer buffer(1 << 14);
    Buffer::Writer writer(&buffer);
    acSent.SerializeRaw(writer);
    Buffer::Reader reader(&buffer);
    T back{};
    back.DeserializeRaw(reader);
    return back;
}

Inventory Of(std::initializer_list<Inventory::Entry> aEntries)
{
    Inventory inventory{};
    for (const auto& acEntry : aEntries)
        inventory.Entries.push_back(acEntry);
    return inventory;
}

int32_t CountOf(const Inventory& acInventory, const std::function<bool(const Inventory::Entry&)>& acWhich)
{
    int32_t count = 0;
    for (const auto& acEntry : acInventory.Entries)
        count += acWhich(acEntry) ? acEntry.Count : 0;
    return count;
}
} // namespace

TEST_CASE("a made potion, a name and a made poison survive the wire with their item", "[item_data]")
{
    NotifyInventoryChanges sent{};
    sent.ServerId = 0x1234;
    sent.OwnershipEpoch = 7;
    sent.Item = MadePotion(3);
    sent.Item.Data.Base.CostOverride = 250;

    auto back = RoundTrip(sent);
    REQUIRE(back == sent);
    REQUIRE(back.Item.Data.Base == sent.Item.Data.Base);
    REQUIRE(back.Item.Data.Base.Effects.size() == 2);
    REQUIRE(back.Item.Data.Base.Effects[1].EffectId == GameId(0, 0x3EB15));
    REQUIRE(back.Item.Data.Base.CostOverride == 250);

    sent.Item = Poisoned(Dagger(1), 3);
    sent.Item.Data.DisplayName = "Lucan's Pride";
    sent.Item.ExtraHealth = 1.3f;
    back = RoundTrip(sent);
    REQUIRE(back == sent);
    REQUIRE(back.Item.Data.DisplayName == "Lucan's Pride");
    REQUIRE(back.Item.Data.Poison.IsPoison);
    REQUIRE(back.Item.ExtraPoisonCount == 3);
}

TEST_CASE("a plain item reads back with no item data, and an entry kept before item data still reads", "[item_data]")
{
    Buffer buffer(1 << 12);
    Buffer::Writer writer(&buffer);
    Dagger(1).Serialize(writer);
    Buffer::Reader reader(&buffer);
    Inventory::Entry back{};
    back.Deserialize(reader);
    REQUIRE(back == Dagger(1));
    REQUIRE_FALSE(back.Data.HasExtra());
    REQUIRE_FALSE(back.Data.Base.IsSet());

    // A server's dropped-item file of version 2: the entry ends at its quest flag, and the item's cell follows.
    Inventory::Entry old = Dagger(2);
    old.ExtraHealth = 1.1f;
    old.IsQuestItem = true;
    Buffer legacy(1 << 12);
    Buffer::Writer legacyWriter(&legacy);
    old.BaseId.Serialize(legacyWriter);
    Serialization::WriteVarInt(legacyWriter, old.Count);
    Serialization::WriteFloat(legacyWriter, old.ExtraCharge);
    old.ExtraEnchantId.Serialize(legacyWriter);
    Serialization::WriteVarInt(legacyWriter, old.ExtraEnchantCharge);
    Serialization::WriteVarInt(legacyWriter, 0);
    Serialization::WriteFloat(legacyWriter, old.ExtraHealth);
    old.ExtraPoisonId.Serialize(legacyWriter);
    Serialization::WriteVarInt(legacyWriter, old.ExtraPoisonCount);
    Serialization::WriteVarInt(legacyWriter, old.ExtraSoulLevel);
    for (const bool cFlag : {false, false, false, false, true})
        Serialization::WriteBool(legacyWriter, cFlag);
    Serialization::WriteVarInt(legacyWriter, 0x16BDB);

    Buffer::Reader legacyReader(&legacy);
    Inventory::Entry legacyBack{};
    legacyBack.Deserialize(legacyReader, false);
    REQUIRE(legacyBack == old);
    REQUIRE(Serialization::ReadVarInt(legacyReader) == 0x16BDB);
}

TEST_CASE("a made item's id is what it does, the same in every game", "[item_data]")
{
    const CreatedAlchemy cPotion = FortifySmithing();
    CreatedAlchemy renamed = cPotion;
    renamed.Name = "Trank der Schmiedekunst";
    renamed.CostOverride = 99;
    CHECK(renamed.Key() == cPotion.Key());

    CreatedAlchemy stronger = cPotion;
    stronger.Effects[0].Magnitude = 13.f;
    CHECK(stronger.Key() != cPotion.Key());

    CreatedAlchemy longer = cPotion;
    longer.Effects[1].Duration = 61;
    CHECK(longer.Key() != cPotion.Key());

    CreatedAlchemy asPoison = cPotion;
    asPoison.IsPoison = true;
    CHECK(asPoison.Key() != cPotion.Key());

    CreatedAlchemy otherEffect = cPotion;
    otherEffect.Effects[0].EffectId = GameId(1, 0x7A102);
    CHECK(otherEffect.Key() != cPotion.Key());

    CHECK(cPotion.Key() != 0);
    CHECK(ItemEffects::Key({}, 0) != 0);
}

TEST_CASE("a message cannot make a game read endless effects or names", "[item_data]")
{
    CreatedAlchemy huge = FortifySmithing();
    huge.Name = String(5000, 'a');
    for (int i = 0; i < 100; ++i)
        huge.Effects.push_back({1.f, 0, 1, 1.f, GameId(0, 0x1000u + i)});

    Buffer buffer(1 << 16);
    Buffer::Writer writer(&buffer);
    huge.Serialize(writer);
    Serialization::WriteVarInt(writer, 77);
    Buffer::Reader reader(&buffer);
    CreatedAlchemy back{};
    back.Deserialize(reader);
    CHECK(back.Name.size() == CreatedAlchemy::kMaxNameLength);
    CHECK(back.Effects.size() == ItemEffects::kMaxEffects);
    // Every effect was read, so what follows is intact.
    CHECK(Serialization::ReadVarInt(reader) == 77);
}

TEST_CASE("a run-time item without what it is is not sent, nor kept", "[item_data]")
{
    CHECK(ItemDataChanges::IsSyncable(Dagger(1)));
    CHECK(ItemDataChanges::IsSyncable(MadePotion(1)));
    CHECK(ItemDataChanges::IsSyncable(Poisoned(Dagger(1), 2)));

    Inventory::Entry bare = MadePotion(1);
    bare.Data.Base = {};
    CHECK_FALSE(ItemDataChanges::IsSyncable(bare));

    Inventory::Entry barePoison = Poisoned(Dagger(1), 2);
    barePoison.Data.Poison = {};
    CHECK_FALSE(ItemDataChanges::IsSyncable(barePoison));
}

TEST_CASE("items with different names or poisons are different stacks", "[item_data]")
{
    Inventory::Entry named = Dagger(1);
    named.Data.DisplayName = "Sting";
    CHECK_FALSE(named.CanBeMerged(Dagger(1)));
    CHECK(named.ContainsExtraData());
    CHECK_FALSE(Poisoned(Dagger(1), 2).CanBeMerged(Dagger(1)));
    CHECK_FALSE(Poisoned(Dagger(1), 2).CanBeMerged(Poisoned(Dagger(1), 1)));
    // A made potion is a form, not extra data: its stack merges like a plain one.
    CHECK_FALSE(MadePotion(1).ContainsExtraData());
    CHECK(MadePotion(1).CanBeMerged(MadePotion(2)));

    Inventory server{};
    server.AddOrRemoveEntry(MadePotion(2));
    server.AddOrRemoveEntry(MadePotion(1));
    REQUIRE(server.Entries.size() == 1);
    CHECK(server.Entries[0].Count == 3);
    server.AddOrRemoveEntry(MadePotion(-3));
    CHECK(server.Entries.empty());
}

TEST_CASE("a weapon tempered where it lies is found as a change", "[item_data]")
{
    Inventory::Entry tempered = Dagger(1);
    tempered.ExtraHealth = 1.2f;

    const auto cChanges = ItemDataChanges::Find(Of({Dagger(2)}), Of({Dagger(1), tempered}));
    REQUIRE(cChanges.size() == 1);
    CHECK(cChanges[0].Old == Dagger(1));
    CHECK(cChanges[0].New == tempered);
}

TEST_CASE("poison put on a worn weapon and used up is found; putting it on or taking it off is not", "[item_data]")
{
    Inventory::Entry worn = Dagger(1);
    worn.ExtraWorn = true;
    const Inventory::Entry cPoisoned = Poisoned(worn, 3);
    const Inventory::Entry cHit = Poisoned(worn, 2);

    // Equipped and unequipped: equipment, sent on its own.
    CHECK(ItemDataChanges::Find(Of({Dagger(1)}), Of({worn})).empty());
    CHECK(ItemDataChanges::Find(Of({worn}), Of({Dagger(1)})).empty());

    auto changes = ItemDataChanges::Find(Of({worn}), Of({cPoisoned}));
    REQUIRE(changes.size() == 1);
    CHECK_FALSE(changes[0].Old.IsWorn());
    CHECK_FALSE(changes[0].New.IsWorn());
    CHECK(changes[0].New.ExtraPoisonCount == 3);
    CHECK(changes[0].New.Data.Poison == Paralysis());

    changes = ItemDataChanges::Find(Of({cPoisoned}), Of({cHit}));
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].Old.ExtraPoisonCount == 3);
    CHECK(changes[0].New.ExtraPoisonCount == 2);

    changes = ItemDataChanges::Find(Of({cHit}), Of({worn}));
    REQUIRE(changes.size() == 1);
    CHECK_FALSE(changes[0].New.ExtraPoisonId);
}

TEST_CASE("an enchanted weapon's charge used in a fight is found", "[item_data]")
{
    Inventory::Entry sword{};
    sword.BaseId = GameId(0, 0x12EB7);
    sword.Count = 1;
    sword.ExtraEnchantId = GameId(0, 0x4605A);
    sword.ExtraCharge = 1000.f;
    sword.ExtraWornLeft = true;
    Inventory::Entry used = sword;
    used.ExtraCharge = 940.f;

    const auto cChanges = ItemDataChanges::Find(Of({sword}), Of({used}));
    REQUIRE(cChanges.size() == 1);
    CHECK(cChanges[0].New.ExtraCharge == 940.f);
    CHECK(ItemDataChanges::Find(Of({sword}), Of({sword})).empty());
}

TEST_CASE("an item that came or went is left to its hook", "[item_data]")
{
    Inventory::Entry tempered = Dagger(1);
    tempered.ExtraHealth = 1.2f;
    // A dagger picked up: the add hook sends it.
    CHECK(ItemDataChanges::Find(Of({Dagger(1)}), Of({Dagger(1), tempered})).empty());
    // A new base: the same.
    CHECK(ItemDataChanges::Find(Of({}), Of({MadePotion(2)})).empty());
    // Gone: the remove hook.
    CHECK(ItemDataChanges::Find(Of({Dagger(2)}), Of({})).empty());
}

TEST_CASE("each stack is paired with what it became", "[item_data]")
{
    Inventory::Entry fine = Dagger(1);
    fine.ExtraHealth = 1.2f;
    Inventory::Entry named = Dagger(1);
    named.Data.DisplayName = "Sting";
    Inventory::Entry namedFine = named;
    namedFine.ExtraHealth = 1.2f;

    const auto cChanges = ItemDataChanges::Find(Of({Dagger(1), named}), Of({fine, namedFine}));
    REQUIRE(cChanges.size() == 2);
    for (const auto& acChange : cChanges)
        CHECK(acChange.Old.Data.DisplayName == acChange.New.Data.DisplayName);
}

TEST_CASE("the server keeps a change with the player's items, where they are worn", "[item_data]")
{
    Inventory::Entry worn = Dagger(1);
    worn.ExtraWorn = true;
    Inventory kept = Of({worn, Dagger(2)});

    ItemDataChange change{ItemDataChanges::Stack(worn), Poisoned(ItemDataChanges::Stack(worn), 3)};
    change.Old.Count = change.New.Count = 2;
    // Both worn and unworn plain daggers are "plain dagger" items.
    REQUIRE(ItemDataChanges::Apply(kept, change) == 2);
    CHECK(CountOf(kept, [](const auto& e) { return e.BaseId == Dagger(1).BaseId; }) == 3);
    CHECK(CountOf(kept, [](const auto& e) { return e.ExtraPoisonCount == 3; }) == 2);
    CHECK(CountOf(kept, [](const auto& e) { return e.ExtraPoisonCount == 3 && e.ExtraWorn; }) == 1);
    CHECK(CountOf(kept, [](const auto& e) { return e.Count < 0; }) == 0);
}

TEST_CASE("a change the server's copy missed repairs it once, and never twice", "[item_data]")
{
    Inventory::Entry tempered = Dagger(1);
    tempered.ExtraHealth = 1.2f;
    Inventory::Entry legendary = Dagger(1);
    legendary.ExtraHealth = 1.6f;

    // The copy has the dagger only as it was before an earlier change it missed.
    Inventory kept = Of({Dagger(1)});
    ItemDataChange change{tempered, legendary};
    REQUIRE(ItemDataChanges::Apply(kept, change) == 1);
    REQUIRE(kept.Entries.size() == 1);
    CHECK(kept.Entries[0] == legendary);

    // Sent again, or already sent by the hooks as a remove and an add: nothing more changes.
    CHECK(ItemDataChanges::Apply(kept, change) == 0);
    REQUIRE(kept.Entries.size() == 1);
    CHECK(kept.Entries[0] == legendary);

    // More changed than the copy holds: no count goes below zero.
    Inventory few = Of({Dagger(1)});
    ItemDataChange many{Dagger(5), tempered};
    many.New.Count = 5;
    CHECK(ItemDataChanges::Apply(few, many) == 1);
    CHECK(CountOf(few, [](const auto& e) { return e.Count < 0; }) == 0);
    CHECK(CountOf(few, [](const auto&) { return true; }) == 1);

    // Another base is never taken.
    Inventory other = Of({MadePotion(4)});
    CHECK(ItemDataChanges::Apply(other, change) == 0);
    CHECK(other.Entries[0] == MadePotion(4));
}

TEST_CASE("a change message survives the wire and keeps at most its cap", "[item_data]")
{
    Inventory::Entry tempered = Dagger(1);
    tempered.ExtraHealth = 1.2f;

    RequestItemDataChange request{};
    request.ServerId = 0x55;
    request.OwnershipEpoch = 3;
    request.Changes.push_back({Dagger(1), tempered});
    request.Changes.push_back({Poisoned(Dagger(1), 2), Poisoned(Dagger(1), 1)});
    CHECK(RoundTrip(request) == request);

    NotifyItemDataChange notify{};
    notify.ServerId = request.ServerId;
    notify.OwnershipEpoch = request.OwnershipEpoch;
    notify.Changes = request.Changes;
    const auto cBack = RoundTrip(notify);
    CHECK(cBack == notify);
    CHECK(cBack.Changes[1].New.Data.Poison == Paralysis());

    for (size_t i = 0; i < 40; ++i)
        request.Changes.push_back({Dagger(1), tempered});
    CHECK(RoundTrip(request).Changes.size() == ItemDataChanges::kMaxChanges);
}

TEST_CASE("VR has the potion and poison makers and the name data vtable, by their SE ids", "[item_data]")
{
    const auto cOffsetOf = [](const uint32_t aId)
    {
        uint32_t found = 0;
        uint32_t times = 0;
        for (const auto& cEntry : kVRAddressOverrides)
        {
            if (cEntry.id == aId)
            {
                found = cEntry.offset;
                ++times;
            }
        }
        return times == 1 ? found : 0u;
    };
    // BGSCreatedObjectManager: AE 36165 to 36168 are SE 35263 to 35266; each pair has the same frame in the exception table.
    CHECK(cOffsetOf(35263) == 0x5a67b0);
    CHECK(cOffsetOf(35264) == 0x5a6850);
    CHECK(cOffsetOf(35265) == 0x5a68f0);
    CHECK(cOffsetOf(35266) == 0x5a69a0);
    // Its RTTI names ExtraTextDisplayData.
    CHECK(cOffsetOf(229625) == 0x15a3e30);
}

TEST_CASE("an item that came and another that went in the same second are not taken for a change", "[item_data]")
{
    Inventory::Entry tempered = Dagger(1);
    tempered.ExtraHealth = 1.2f;
    // One plain dagger dropped and a tempered one picked up: the hooks said both.
    const Inventory cExpected = ItemDataChanges::Expect(Of({Dagger(2)}), {Dagger(-1), tempered});
    CHECK(ItemDataChanges::Find(cExpected, Of({Dagger(1), tempered})).empty());
    // Without what the hooks said it would look like a temper.
    CHECK(ItemDataChanges::Find(Of({Dagger(2)}), Of({Dagger(1), tempered})).size() == 1);

    // A temper in the same second as an unrelated pick-up is still found.
    Inventory::Entry fine = Dagger(1);
    fine.ExtraHealth = 1.1f;
    const Inventory cWithPotion = ItemDataChanges::Expect(Of({Dagger(1)}), {MadePotion(1)});
    const auto cChanges = ItemDataChanges::Find(cWithPotion, Of({fine}));
    REQUIRE(cChanges.size() == 1);
    CHECK(cChanges[0].New == fine);
}

TEST_CASE("the server repairs a missed change only from the same item, not from another of its base", "[item_data]")
{
    Inventory::Entry named = Dagger(1);
    named.Data.DisplayName = "Sting";
    Inventory::Entry tempered = Dagger(1);
    tempered.ExtraHealth = 1.2f;
    Inventory::Entry legendary = Dagger(1);
    legendary.ExtraHealth = 1.6f;

    // The kept copy has only the player's named dagger: a plain dagger's temper is not put on it.
    Inventory kept = Of({named});
    CHECK(ItemDataChanges::Apply(kept, {tempered, legendary}) == 0);
    REQUIRE(kept.Entries.size() == 1);
    CHECK(kept.Entries[0] == named);
}

TEST_CASE("a made poison is told by what it does, not by the words a game named it with", "[item_data]")
{
    Inventory::Entry here = Poisoned(Dagger(1), 2);
    Inventory::Entry there = here;
    there.Data.Poison.Name = "Gift der Paralyse";
    CHECK(here.CanBeMerged(there));
    Inventory::Entry stronger = here;
    stronger.Data.Poison.Effects[0].Magnitude = 2.f;
    stronger.ExtraPoisonId = GameId(kRunTime, stronger.Data.Poison.Key());
    CHECK_FALSE(here.CanBeMerged(stronger));
}

TEST_CASE("the server marks the very stack worn, not the plain one of the same base", "[item_data]")
{
    Inventory::Entry enchanted = Dagger(1);
    enchanted.ExtraEnchantId = GameId(0, 0x4605A);
    Inventory kept = Of({Dagger(1), enchanted});

    Inventory::Entry worn = enchanted;
    worn.ExtraWorn = true;
    kept.UpdateEquipment(Of({worn}));
    CHECK_FALSE(kept.Entries[0].IsWorn());
    CHECK(kept.Entries[1].ExtraWorn);
}

namespace
{
ItemEffect RoundTrip(const ItemEffect& acSent)
{
    Buffer buffer(256);
    Buffer::Writer writer(&buffer);
    acSent.Serialize(writer);
    Buffer::Reader reader(&buffer);
    ItemEffect back{};
    back.Deserialize(reader);
    return back;
}
} // namespace

TEST_CASE("an honest effect passes the decode unchanged", "[item_data]")
{
    DecodeGuard::t_failed = false;
    const ItemEffect sent{12.5f, 10, 30, 41.25f, GameId(0, 0x7A102)};
    CHECK(RoundTrip(sent) == sent);
    CHECK_FALSE(DecodeGuard::t_failed);
}

TEST_CASE("an effect whose magnitude or cost is not a number is zeroed and the message is kept", "[item_data]")
{
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()})
    {
        DecodeGuard::t_failed = false;
        ItemEffect sent{1.f, 0, 30, 2.f, GameId(0, 0x7A102)};
        sent.Magnitude = bad;
        const ItemEffect back = RoundTrip(sent);
        CHECK_FALSE(DecodeGuard::t_failed);
        CHECK(back.Magnitude == 0.f);
        CHECK(back.RawCost == 2.f);
        CHECK(back.Duration == 30);

        DecodeGuard::t_failed = false;
        sent = ItemEffect{1.f, 0, 30, 2.f, GameId(0, 0x7A102)};
        sent.RawCost = bad;
        const ItemEffect backCost = RoundTrip(sent);
        CHECK_FALSE(DecodeGuard::t_failed);
        CHECK(backCost.RawCost == 0.f);
        CHECK(backCost.Magnitude == 1.f);
    }
    DecodeGuard::t_failed = false;
}

TEST_CASE("an effect beyond what any game holds is brought within it", "[item_data]")
{
    DecodeGuard::t_failed = false;
    ItemEffect sent{3.0e30f, 0, 0, -3.0e30f, GameId(0, 0x7A102)};
    sent.Area = std::numeric_limits<int32_t>::max();
    sent.Duration = -5;
    const ItemEffect back = RoundTrip(sent);
    CHECK_FALSE(DecodeGuard::t_failed);
    CHECK(back.Magnitude == ItemEffects::kMaxValue);
    CHECK(back.RawCost == -ItemEffects::kMaxValue);
    CHECK(back.Area == ItemEffects::kMaxSpan);
    CHECK(back.Duration == 0);
}

TEST_CASE("an enchantment with more effects than any item has keeps the first ones", "[item_data]")
{
    Inventory::Entry sent = Dagger(1);
    sent.ExtraEnchantId = GameId(kRunTime, 7);
    for (uint32_t i = 0; i < 100; ++i)
        sent.EnchantData.Effects.push_back({static_cast<float>(i), 0, 30, 1.f, GameId(0, 0x7A102 + i)});

    Buffer buffer(1 << 14);
    Buffer::Writer writer(&buffer);
    sent.Serialize(writer);
    Buffer::Reader reader(&buffer);
    Inventory::Entry back{};
    back.Deserialize(reader, true);
    REQUIRE(back.EnchantData.Effects.size() == ItemEffects::kMaxEffects);
    CHECK(back.EnchantData.Effects.front().Magnitude == 0.f);
    CHECK(back.EnchantData.Effects.back().Magnitude == static_cast<float>(ItemEffects::kMaxEffects - 1));
}

TEST_CASE("a copy's saved owner list follows the owner's equipment changes", "[item_data]")
{
    // At the copy's spawn its owner wore a bow and held a flames spell.
    Inventory::Entry bow{};
    bow.BaseId = GameId(0, 0x13985);
    bow.Count = 1;
    bow.ExtraWorn = true;
    Inventory::Entry sword = Dagger(1);
    Inventory saved = Of({bow, sword});
    saved.CurrentMagicEquipment.RightHandSpell = GameId(0, 0x12FCD);

    // A second later the owner draws the sword and casts nothing: its equipment message names what it wears now.
    Inventory::Entry wornSword = sword;
    wornSword.ExtraWorn = true;
    Inventory now = Of({wornSword});
    saved.UpdateEquipment(now);

    // A later set from the saved list puts the sword in the hand, not the bow, and no spell.
    CHECK_FALSE(saved.Entries[0].IsWorn());
    CHECK(saved.Entries[1].ExtraWorn);
    CHECK(saved.CurrentMagicEquipment.RightHandSpell == GameId{});
    // The items themselves stay: equipment changes only what is worn.
    CHECK(saved.Entries.size() == 2);
}
