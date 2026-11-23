#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <glm/vec3.hpp>
#include <Services/DroppedItemReplies.h>
#include <catch2/catch.hpp>
#include <map>
#include <unordered_set>

namespace
{
//! Disposable inventory and scene adapters for the receive pipeline used by DroppedItemService.
struct Drops
{
    std::vector<DroppedItemIdentity::Pending> Pending;
    std::vector<DroppedItemReplies::AwaitingScene> Awaiting;
    std::map<uint32_t, Inventory::Entry> Floor;
    std::map<uint32_t, uint32_t> Mapped;
    std::map<uint32_t, uint32_t> MappedHandles;
    uint32_t CurrentHandle{70};
    int Announcements{};
    std::vector<uint32_t> Removed;
    std::unordered_set<uint32_t> Removing;
    Inventory::Entry Item{};
    bool Loaded{true};
    bool HandleLive{true};
    int InventoryCount{};
    int Placements{};
    int LoadChecks{};
    int SceneReads{};

    Drops()
    {
        Item.BaseId = GameId(0, 0x12eb7);
        Item.Count = 1;
        Item.Data.DisplayName = "Disposable drop";
        Item.ExtraHealth = 1.3f;
        DroppedItemIdentity::Pending drop{};
        drop.RefFormId = 7;
        drop.RefHandle = 70;
        drop.RequestId = 11;
        drop.Item = Item;
        drop.Item.Count = -1;
        drop.CellId = GameId(0, 0xabc);
        Pending.push_back(drop);
        Floor.emplace(7, Item);
    }

    NotifyDroppedItem Reply() const
    {
        NotifyDroppedItem message{};
        message.Id = 42;
        message.RequestId = 11;
        message.Item = Item;
        message.CellId = GameId(0, 0xabc);
        return message;
    }

    void Remove(uint32_t aId)
    {
        if (Removing.insert(aId).second)
            Removed.push_back(aId);
    }

    bool Pickup(uint32_t aHandle = 70, bool aActualPickup = true)
    {
        if (aActualPickup)
        {
            Floor.erase(7);
            ++InventoryCount;
        }
        if (Mapped.contains(42))
        {
            Remove(42);
            Mapped.erase(42);
            return true;
        }
        return DroppedItemReplies::PickedUpScoped(
            Pending, Awaiting, 7, aHandle, Item, GameId(0, 0xabc), {}, {}, 200.f, [&](uint32_t) { return HandleLive; }, [&](uint32_t aId) { Remove(aId); });
    }

    void CellChanged()
    {
        AnnounceOldDrops();
        // Runner drains already-queued pickups before this queued local replay pass.
        for (const auto& message : DroppedItemReplies::Replays(Awaiting))
            Receive(message);
    }

    void AnnounceOldDrops()
    {
        for (const auto& [id, item] : Floor)
            if (!DroppedItemReplies::Reserved(Pending, Awaiting, id) && std::none_of(Mapped.begin(), Mapped.end(), [id](const auto& acMap) { return acMap.second == id; }))
                ++Announcements;
    }

    DroppedItemReplies::Result Receive(NotifyDroppedItem aMessage)
    {
        if (Removing.contains(aMessage.Id))
            return {DroppedItemReplies::Outcome::Handled, false};
        if (Mapped.contains(aMessage.Id))
        {
            const auto floor = Floor.find(Mapped.at(aMessage.Id));
            if (floor != Floor.end() && DroppedItemReplies::MappedMatches(MappedHandles.at(aMessage.Id), CurrentHandle, aMessage.Item, floor->second, aMessage.LegacyItemData))
                return {DroppedItemReplies::Outcome::Handled, false};
            Mapped.erase(aMessage.Id);
            MappedHandles.erase(aMessage.Id);
            aMessage.Replay = true;
        }
        const auto result = DroppedItemReplies::Receive(
            Pending, Awaiting, aMessage,
            [&](const auto& acPending)
            {
                ++SceneReads;
                const auto it = Floor.find(acPending.RefFormId);
                if (!HandleLive || it == Floor.end() || !DroppedItemIdentity::SameStack(it->second, aMessage.Item))
                    return false;
                Mapped[aMessage.Id] = acPending.RefFormId;
                MappedHandles[aMessage.Id] = CurrentHandle;
                std::erase_if(Awaiting, [&](const auto& acWaiting) { return acWaiting.ServerId == aMessage.Id; });
                return true;
            },
            [&](uint32_t aId) { Remove(aId); },
            [&]
            {
                ++LoadChecks;
                return Loaded;
            });
        if (result.Action != DroppedItemReplies::Outcome::Place || Mapped.contains(aMessage.Id))
            return result;
        if (std::any_of(
                Pending.begin(), Pending.end(),
                [&](const auto& acPending)
                {
                    return DroppedItemIdentity::WaitingForReplay(
                        acPending, aMessage.Item, aMessage.Replay, aMessage.LegacyItemData, aMessage.CellId, aMessage.WorldSpaceId, aMessage.Position, 200.f);
                }))
            return result;
        DroppedItemIdentity::Nearby nearby(aMessage.Item, aMessage.LegacyItemData, 200.f, aMessage.Replay);
        for (const auto& [id, item] : Floor)
            nearby.Consider(id, item, 0.f);
        if (nearby.Get().Id)
            Mapped[aMessage.Id] = nearby.Get().Id;
        else
        {
            Floor.emplace(100 + Placements, aMessage.Item);
            Mapped[aMessage.Id] = 100 + Placements++;
        }
        MappedHandles[aMessage.Id] = CurrentHandle;
        std::erase_if(Awaiting, [&](const auto& acWaiting) { return acWaiting.ServerId == aMessage.Id; });
        return result;
    }
};
} // namespace

TEST_CASE("Drop pickup and acknowledgement orders preserve one inventory item", "[droppeditems][droppeditem_service]")
{
    for (const bool pickupFirst : {true, false})
    {
        Drops drops;
        if (pickupFirst)
            REQUIRE(drops.Pickup());
        const auto result = drops.Receive(drops.Reply());
        CHECK(result.Consumed);
        if (!pickupFirst)
            REQUIRE(drops.Pickup());
        CHECK(drops.Pending.empty());
        CHECK(drops.Floor.empty());
        CHECK(drops.InventoryCount == 1);
        CHECK(drops.Placements == 0);
        REQUIRE(drops.Removed.size() == 1);
        CHECK(drops.Removed.front() == 42);

        // A replay queued before the server processes the removal cannot put the stack back.
        auto replay = drops.Reply();
        replay.RequestId = 0;
        replay.Replay = true;
        drops.Receive(replay);
        CHECK(drops.Floor.empty());
        CHECK(drops.Placements == 0);
        CHECK(drops.Removed.size() == 1);
    }
}

TEST_CASE("An own drop reply is consumed outside its loaded cell", "[droppeditems][droppeditem_service]")
{
    for (const bool referenceAvailable : {true, false})
    {
        Drops drops;
        drops.AnnounceOldDrops();
        CHECK(drops.Announcements == 0);
        drops.Loaded = false;
        drops.HandleLive = referenceAvailable;
        auto result = drops.Receive(drops.Reply());
        CHECK(result.Consumed);
        CHECK(drops.Pending.empty());
        CHECK(drops.Placements == 0);
        CHECK(drops.Removed.empty());
        CHECK(drops.Mapped.empty());
        CHECK(drops.SceneReads == 0);
        CHECK(result.Action == DroppedItemReplies::Outcome::Unloaded);
        REQUIRE(drops.Awaiting.size() == 1);
        drops.AnnounceOldDrops();
        CHECK(drops.Announcements == 0);

        drops.Loaded = true;
        // The old handle may no longer resolve after its cell reloads.
        drops.CurrentHandle = 71;
        drops.HandleLive = false;
        auto replay = drops.Reply();
        replay.RequestId = 0;
        replay.Replay = true;
        drops.Receive(replay);
        CHECK(drops.Mapped.at(42) == 7);
        CHECK(drops.Awaiting.empty());
        CHECK(drops.Floor.size() == 1);
        CHECK(drops.Placements == 0);
        drops.AnnounceOldDrops();
        CHECK(drops.Announcements == 0);
    }
}

TEST_CASE("Pickup remains known across an unloaded acknowledgement", "[droppeditems][droppeditem_service]")
{
    for (const bool pickupFirst : {true, false})
    {
        Drops drops;
        drops.Loaded = false;
        drops.HandleLive = false;
        if (pickupFirst)
            REQUIRE(drops.Pickup());
        drops.Receive(drops.Reply());
        if (!pickupFirst)
            REQUIRE(drops.Pickup());
        CHECK(drops.Pending.empty());
        CHECK(drops.Awaiting.empty());
        CHECK(drops.InventoryCount == 1);
        CHECK(drops.Placements == 0);
        REQUIRE(drops.Removed.size() == 1);
        CHECK(drops.Removed.front() == 42);
        CHECK(drops.LoadChecks == (pickupFirst ? 0 : 1));
    }
}

TEST_CASE("Only the exact ordinary stack consumes a pending ownership reply", "[droppeditems][droppeditem_service]")
{
    Drops drops;
    drops.Loaded = false;
    CHECK_FALSE(drops.Pickup(71, false));
    CHECK_FALSE(drops.Pending.front().PickedUp);
    auto wrong = drops.Reply();
    wrong.Item.Data.DisplayName = "Another dagger";
    CHECK_FALSE(drops.Receive(wrong).Consumed);
    wrong = drops.Reply();
    wrong.RequestId = 12;
    CHECK_FALSE(drops.Receive(wrong).Consumed);
    CHECK(drops.Pending.size() == 1);
    CHECK(drops.Awaiting.empty());
    CHECK(drops.Removed.empty());
}

TEST_CASE("A missing own floor item is placed only after its scene loads", "[droppeditems][droppeditem_service]")
{
    Drops drops;
    drops.Loaded = false;
    drops.HandleLive = false;
    drops.Floor.clear();
    drops.Receive(drops.Reply());
    CHECK(drops.Pending.empty());
    CHECK(drops.Placements == 0);
    drops.Loaded = true;
    auto replay = drops.Reply();
    replay.RequestId = 0;
    replay.Replay = true;
    drops.Receive(replay);
    CHECK(drops.Awaiting.empty());
    CHECK(drops.Floor.size() == 1);
    CHECK(drops.Placements == 1);
    CHECK(drops.InventoryCount == 0);
    CHECK(drops.Removed.empty());
}

TEST_CASE("A stale mapped handle repairs through verified replay adoption", "[droppeditems][droppeditem_service]")
{
    for (const bool sameStack : {true, false})
    {
        Drops drops;
        drops.Receive(drops.Reply());
        REQUIRE(drops.Mapped.at(42) == 7);
        drops.CurrentHandle = 71;
        drops.HandleLive = false;
        if (!sameStack)
            drops.Floor.at(7).ExtraHealth = 2.f;
        auto replay = drops.Reply();
        replay.RequestId = 0;
        replay.Replay = true;
        drops.Receive(replay);
        CHECK(drops.Placements == (sameStack ? 0 : 1));
        CHECK(drops.Mapped.at(42) == (sameStack ? 7 : 100));
        CHECK(drops.Floor.size() == (sameStack ? 1 : 2));
        CHECK(drops.Removed.empty());
    }
}

TEST_CASE("A replay received before cell load retries locally without another server message", "[droppeditems][droppeditem_service]")
{
    for (const bool floorSurvives : {true, false})
    {
        Drops drops;
        drops.Loaded = false;
        drops.HandleLive = false;
        drops.Receive(drops.Reply());
        auto replay = drops.Reply();
        replay.RequestId = 0;
        replay.Replay = true;
        replay.Rotation = glm::vec3(0.1f, 0.2f, 0.3f);
        drops.Receive(replay);
        REQUIRE(drops.Awaiting.size() == 1);
        CHECK(drops.Awaiting.front().Message.Rotation == replay.Rotation);
        CHECK(drops.SceneReads == 0);
        if (!floorSurvives)
            drops.Floor.clear();
        drops.Loaded = true;
        drops.CurrentHandle = 71;
        drops.CellChanged();
        CHECK(drops.Pending.empty());
        CHECK(drops.Awaiting.empty());
        CHECK(drops.Mapped.size() == 1);
        CHECK(drops.Floor.size() == 1);
        CHECK(drops.Placements == (floorSurvives ? 0 : 1));
        CHECK(drops.Announcements == 0);
        CHECK(drops.Removed.empty());
    }
}

TEST_CASE("A queued pickup after handle reload precedes the local cell retry", "[droppeditems][droppeditem_service]")
{
    for (const bool acknowledged : {true, false})
    {
        Drops drops;
        drops.Loaded = false;
        drops.HandleLive = false;
        if (acknowledged)
            drops.Receive(drops.Reply());
        drops.Loaded = true;
        drops.CurrentHandle = 71;
        REQUIRE(drops.Pickup(71));
        drops.CellChanged();
        if (!acknowledged)
            drops.Receive(drops.Reply());
        CHECK(drops.Pending.empty());
        CHECK(drops.Awaiting.empty());
        CHECK(drops.Floor.empty());
        CHECK(drops.Placements == 0);
        CHECK(drops.InventoryCount == 1);
        REQUIRE(drops.Removed.size() == 1);
        CHECK(drops.Removed.front() == 42);
    }
}

TEST_CASE("Reload pickup matching requires dead handle original reference stack and scoped place", "[droppeditems][droppeditem_service]")
{
    for (int mismatch = 0; mismatch != 7; ++mismatch)
    {
        Drops drops;
        drops.Loaded = false;
        drops.HandleLive = false;
        drops.Receive(drops.Reply());
        GameId cell(0, mismatch == 1 ? 0xdef : 0xabc);
        GameId world = mismatch == 2 ? GameId(0, 0x123) : GameId{};
        glm::vec3 position(mismatch == 3 ? 201.f : 0.f, 0.f, 0.f);
        auto item = drops.Item;
        if (mismatch == 4)
            item.Data.DisplayName = "Different stack";
        if (mismatch == 6)
            item.Count = 2;
        const auto ref = mismatch == 5 ? 8u : 7u;
        CHECK_FALSE(DroppedItemReplies::PickedUpScoped(
            drops.Pending, drops.Awaiting, ref, 71, item, cell, world, position, 200.f, [&](uint32_t) { return mismatch == 0; }, [&](uint32_t aId) { drops.Remove(aId); }));
        CHECK(drops.Removed.empty());
        CHECK(drops.Awaiting.size() == 1);
    }
}

TEST_CASE("A pick-up elsewhere removes only the item still under its mapped id", "[droppeditems][droppeditem_service]")
{
    using DroppedItemReplies::StillTheItem;
    // The same item: same base, same handle.
    CHECK(StillTheItem(0x12EB7, 0x00100A01, 0x12EB7, 0x00100A01, false));
    // The id given to another object since: another base, or the same base under a new handle.
    CHECK_FALSE(StillTheItem(0x12EB7, 0x00100A01, 0x1BCB2, 0x00200B02, false));
    CHECK_FALSE(StillTheItem(0x12EB7, 0x00100A01, 0x12EB7, 0x00300C03, false));
    // An actor is never removed as an item.
    CHECK_FALSE(StillTheItem(0x12EB7, 0x00100A01, 0x12EB7, 0x00100A01, true));
    CHECK_FALSE(StillTheItem(0, 0, 0x7, 0x00100A01, true));
    // Nothing known of the mapping: only the actor rule applies.
    CHECK(StillTheItem(0, 0, 0x12EB7, 0x00100A01, false));
}
