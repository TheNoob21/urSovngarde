#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <catch2/catch.hpp>

#include <Messages/RequestDroppedItemAdd.h>
#include <Messages/RequestDroppedItemRemove.h>
#include <Messages/NotifyDroppedItem.h>
#include <Messages/NotifyDroppedItemRemoved.h>
#include <Messages/RequestDroppedItemMove.h>
#include <Messages/NotifyDroppedItemMove.h>
#include <Messages/RequestWorldObjectMove.h>
#include <Messages/NotifyWorldObjectMove.h>

#include <Services/ReferenceAngles.h>
#include <DroppedItemIdentity.h>
#include <DroppedItemRecord.h>
#include <DecodeGuard.h>
#include <map>

using namespace TiltedPhoques;

// The messages behind remembered drops and moved items (2026-09-30). The drops bot pair sends three of them end to end; the
// Announcement flag -- how a player's earlier drops are told apart from a fresh one -- only travels when the game reads
// the player's dropped-item list, which no bot can do, so it is checked here.

namespace
{
Inventory::Entry EnchantedDagger()
{
    Inventory::Entry item{};
    item.BaseId = GameId(0, 0x12EB7);
    item.Count = -1;
    item.ExtraHealth = 1.3f;
    item.ExtraEnchantId = GameId(0, 0x4605A);
    item.ExtraEnchantCharge = 500;
    return item;
}

void WriteOldRecord(Buffer::Writer& aWriter, const DroppedItemRecord& acItem, uint64_t aVersion)
{
    Serialization::WriteVarInt(aWriter, acItem.Id);
    const auto& item = acItem.Entry;
    if (aVersion >= 3)
        item.Serialize(aWriter);
    else
    {
        item.BaseId.Serialize(aWriter);
        Serialization::WriteVarInt(aWriter, item.Count);
        Serialization::WriteFloat(aWriter, item.ExtraCharge);
        item.ExtraEnchantId.Serialize(aWriter);
        Serialization::WriteVarInt(aWriter, item.ExtraEnchantCharge);
        Serialization::WriteVarInt(aWriter, item.EnchantData.Effects.size());
        for (const auto& effect : item.EnchantData.Effects)
            effect.Serialize(aWriter);
        Serialization::WriteFloat(aWriter, item.ExtraHealth);
        item.ExtraPoisonId.Serialize(aWriter);
        Serialization::WriteVarInt(aWriter, item.ExtraPoisonCount);
        Serialization::WriteVarInt(aWriter, item.ExtraSoulLevel);
        for (bool flag : {item.EnchantData.IsWeapon, item.ExtraEnchantRemoveUnequip, item.ExtraWorn, item.ExtraWornLeft, item.IsQuestItem})
            Serialization::WriteBool(aWriter, flag);
    }
    acItem.CellId.Serialize(aWriter);
    acItem.WorldSpaceId.Serialize(aWriter);
    acItem.Position.Serialize(aWriter);
    if (aVersion >= 2)
    {
        Serialization::WriteFloat(aWriter, acItem.Rotation.x);
        Serialization::WriteFloat(aWriter, acItem.Rotation.y);
        Serialization::WriteFloat(aWriter, acItem.Rotation.z);
    }
}
} // namespace

TEST_CASE("a drop and an announcement survive the wire, and stay different", "[droppeditems]")
{
    for (const bool announcement : {false, true})
    {
        RequestDroppedItemAdd sent{};
        sent.Item = EnchantedDagger();
        sent.CellId = GameId(0, 0x16BDB);
        sent.WorldSpaceId = GameId(0, 0x3C);
        sent.Position = glm::vec3(141472.5f, -27222.25f, -13092.125f);
        sent.Rotation = glm::vec3(0.1f, -1.5f, 6.2f);
        sent.Announcement = announcement;

        Buffer buffer(1 << 12);
        Buffer::Writer writer(&buffer);
        sent.SerializeRaw(writer);
        Buffer::Reader reader(&buffer);
        RequestDroppedItemAdd back{};
        back.DeserializeRaw(reader);

        INFO("announcement " << announcement);
        REQUIRE(back.Announcement == announcement);
        REQUIRE(back.Item == sent.Item);
        REQUIRE(back.CellId == sent.CellId);
        REQUIRE(back.WorldSpaceId == sent.WorldSpaceId);
        REQUIRE(back.Position == sent.Position);
        REQUIRE(back.Rotation == sent.Rotation);
    }
}

TEST_CASE("the server's copy of an item carries its enchantment and its id", "[droppeditems]")
{
    NotifyDroppedItem sent{};
    sent.Id = 4242;
    sent.Item = EnchantedDagger();
    sent.Item.Count = 3;
    sent.CellId = GameId(0, 0x16BDB);
    sent.Position = glm::vec3(1.f, 2.f, 3.f);
    sent.Rotation = glm::vec3(0.f, 0.f, 3.14159f);

    Buffer buffer(1 << 12);
    Buffer::Writer writer(&buffer);
    sent.SerializeRaw(writer);
    Buffer::Reader reader(&buffer);
    NotifyDroppedItem back{};
    back.DeserializeRaw(reader);

    REQUIRE(back.Id == 4242);
    REQUIRE(back.Item == sent.Item);
    REQUIRE(back.Item.ExtraEnchantId == GameId(0, 0x4605A));
    REQUIRE(back.WorldSpaceId == GameId{}); // an interior: no worldspace
    REQUIRE(back.Rotation == sent.Rotation);
}

TEST_CASE("an add's request id comes back on the wire with the item, and an add without one stays without", "[droppeditems]")
{
    for (const uint32_t requestId : {0u, 1u, 300u, 0xFFFFFFFFu})
    {
        INFO("request id " << requestId);

        RequestDroppedItemAdd add{};
        add.Item = EnchantedDagger();
        add.CellId = GameId(0, 0x16BDB);
        add.Position = glm::vec3(1.f, 2.f, 3.f);
        add.Rotation = glm::vec3(0.f, 0.f, 1.25f);
        add.RequestId = requestId;

        Buffer addBuffer(1 << 12);
        Buffer::Writer addWriter(&addBuffer);
        add.SerializeRaw(addWriter);
        Buffer::Reader addReader(&addBuffer);
        RequestDroppedItemAdd addBack{};
        addBack.DeserializeRaw(addReader);
        REQUIRE(addBack == add);
        REQUIRE(addBack.RequestId == requestId);
        REQUIRE_FALSE(addBack.Announcement);

        NotifyDroppedItem reply{};
        reply.Id = 4242;
        reply.Item = add.Item;
        reply.CellId = add.CellId;
        reply.Position = add.Position;
        reply.Rotation = add.Rotation;
        reply.RequestId = requestId;

        Buffer replyBuffer(1 << 12);
        Buffer::Writer replyWriter(&replyBuffer);
        reply.SerializeRaw(replyWriter);
        Buffer::Reader replyReader(&replyBuffer);
        NotifyDroppedItem replyBack{};
        replyBack.DeserializeRaw(replyReader);
        REQUIRE(replyBack == reply);
        REQUIRE(replyBack.RequestId == requestId);
        REQUIRE(replyBack.Id == 4242);
        REQUIRE(replyBack.Rotation == reply.Rotation);
    }

    // Unset, both carry 0: an ordinary drop and every other player's copy.
    REQUIRE(RequestDroppedItemAdd{}.RequestId == 0);
    REQUIRE(NotifyDroppedItem{}.RequestId == 0);
}

TEST_CASE("a pick-up names the item by id alone", "[droppeditems]")
{
    for (const uint32_t id : {1u, 300u, 0xFFFFFFFFu})
    {
        RequestDroppedItemRemove request{};
        request.Id = id;
        Buffer buffer(64);
        Buffer::Writer writer(&buffer);
        request.SerializeRaw(writer);
        Buffer::Reader reader(&buffer);
        RequestDroppedItemRemove back{};
        back.DeserializeRaw(reader);
        REQUIRE(back.Id == id);

        NotifyDroppedItemRemoved notify{};
        notify.Id = id;
        Buffer buffer2(64);
        Buffer::Writer writer2(&buffer2);
        notify.SerializeRaw(writer2);
        Buffer::Reader reader2(&buffer2);
        NotifyDroppedItemRemoved back2{};
        back2.DeserializeRaw(reader2);
        REQUIRE(back2.Id == id);
    }
}

TEST_CASE("a move keeps its turn exactly and says whether the item was put down", "[droppeditems]")
{
    // The turn goes as three floats, not packed like the position: a packed angle loses a few degrees, which an item
    // lying on a table shows at once.
    for (const bool atRest : {false, true})
    {
        RequestDroppedItemMove request{};
        request.Id = 77;
        request.Position = glm::vec3(-5120.f, 20480.5f, 300.25f);
        request.Rotation = glm::vec3(0.0123f, 3.0f, 6.28f);
        request.AtRest = atRest;

        Buffer buffer(256);
        Buffer::Writer writer(&buffer);
        request.SerializeRaw(writer);
        Buffer::Reader reader(&buffer);
        RequestDroppedItemMove back{};
        back.DeserializeRaw(reader);

        INFO("at rest " << atRest);
        REQUIRE(back.Id == 77);
        REQUIRE(back.Position == request.Position);
        REQUIRE(back.Rotation == request.Rotation);
        REQUIRE(back.AtRest == atRest);

        NotifyDroppedItemMove notify{};
        notify.Id = 0xFFFFFFFFu;
        notify.Position = request.Position;
        notify.Rotation = request.Rotation;
        notify.AtRest = atRest;

        Buffer buffer2(256);
        Buffer::Writer writer2(&buffer2);
        notify.SerializeRaw(writer2);
        Buffer::Reader reader2(&buffer2);
        NotifyDroppedItemMove back2{};
        back2.DeserializeRaw(reader2);

        REQUIRE(back2.Id == 0xFFFFFFFFu);
        REQUIRE(back2.Position == notify.Position);
        REQUIRE(back2.Rotation == notify.Rotation);
        REQUIRE(back2.AtRest == atRest);
    }
}

namespace
{
// A reference's 3D as the game turns it from its angles: Rx(-x) * Ry(-y) * Rz(-z), each clockwise.
glm::mat3 TurnOf(const glm::vec3& acAngles)
{
    glm::mat4 turn(1.f);
    turn = glm::rotate(turn, -acAngles.x, glm::vec3(1.f, 0.f, 0.f));
    turn = glm::rotate(turn, -acAngles.y, glm::vec3(0.f, 1.f, 0.f));
    turn = glm::rotate(turn, -acAngles.z, glm::vec3(0.f, 0.f, 1.f));
    return glm::mat3(turn);
}

bool SameTurn(const glm::mat3& acA, const glm::mat3& acB)
{
    for (int column = 0; column < 3; ++column)
        for (int row = 0; row < 3; ++row)
            if (std::abs(acA[column][row] - acB[column][row]) > 1e-4f)
                return false;
    return true;
}
} // namespace

// A held item is sent with the angles of its 3D's turn (2026-11-15): a VR hand turned it every way, and the other
// player saw it stay flat. The angles must give the same turn back once the other game puts them on the reference.
TEST_CASE("an item's turn is sent as angles that turn it the same way again", "[droppeditems]")
{
    constexpr float cQuarter = glm::half_pi<float>();

    SECTION("heading and pitch turn the way the game turns them")
    {
        // Heading 90 degrees faces east (X); pitch 90 degrees faces down. Y is forward.
        const glm::vec3 east = TurnOf({0.f, 0.f, cQuarter})[1];
        REQUIRE(east.x == Approx(1.f).margin(1e-5));
        REQUIRE(east.y == Approx(0.f).margin(1e-5));
        const glm::vec3 down = TurnOf({cQuarter, 0.f, 0.f})[1];
        REQUIRE(down.z == Approx(-1.f).margin(1e-5));

        const glm::vec3 angles = ReferenceAngles::FromTurn(TurnOf({0.f, 0.f, cQuarter}));
        REQUIRE(angles.x == Approx(0.f).margin(1e-5));
        REQUIRE(angles.y == Approx(0.f).margin(1e-5));
        REQUIRE(angles.z == Approx(cQuarter).margin(1e-5));
    }

    SECTION("a mix of all three angles comes back as the angles the game holds")
    {
        // An item the game held at 80, 30 and 90 degrees: read in the other order, it came back as -30, 80 and 90.
        const glm::vec3 cHeld(1.3963f, 0.5236f, 1.5708f);
        const glm::vec3 angles = ReferenceAngles::FromTurn(TurnOf(cHeld));
        REQUIRE(angles.x == Approx(cHeld.x).margin(1e-4));
        REQUIRE(angles.y == Approx(cHeld.y).margin(1e-4));
        REQUIRE(angles.z == Approx(cHeld.z).margin(1e-4));
    }

    SECTION("every mix of the three angles comes back as the same turn")
    {
        const float cSteps[] = {-3.f, -2.2f, -1.4f, -0.6f, 0.f, 0.3f, 1.1f, 1.9f, 2.7f};
        for (const float x : cSteps)
            for (const float y : cSteps)
                for (const float z : cSteps)
                {
                    const glm::mat3 turn = TurnOf({x, y, z});
                    const glm::vec3 angles = ReferenceAngles::FromTurn(turn);
                    INFO("angles " << x << ", " << y << ", " << z << " came back as " << angles.x << ", " << angles.y << ", " << angles.z);
                    REQUIRE(SameTurn(TurnOf(angles), turn));
                }
    }

    SECTION("an item picked up flat from the ground is sent turned as the hand turns it")
    {
        // A tunic lying flat, heading 1.2 rad: the reference keeps these angles while a hand holds it.
        const glm::vec3 cResting(0.f, 0.f, 1.2f);
        const glm::mat3 resting = TurnOf(cResting);

        // The hand lifts it and turns it on edge: about the world's X, then Y.
        glm::mat4 hand(1.f);
        hand = glm::rotate(hand, 1.1f, glm::vec3(0.f, 1.f, 0.f));
        hand = glm::rotate(hand, -0.8f, glm::vec3(1.f, 0.f, 0.f));
        const glm::mat3 held = glm::mat3(hand) * resting;

        const glm::vec3 angles = ReferenceAngles::FromTurn(held);
        REQUIRE(SameTurn(TurnOf(angles), held));
        REQUIRE_FALSE(SameTurn(TurnOf(angles), resting));
        // No longer flat: its own up no longer points at the sky.
        REQUIRE(TurnOf(angles)[2].z < 0.9f);
    }

    SECTION("an item standing straight up or down keeps its turn")
    {
        for (const float y : {cQuarter, -cQuarter})
            for (const float x : {-1.f, 0.f, 0.7f})
            {
                const glm::mat3 turn = TurnOf({x, y, 2.f});
                REQUIRE(SameTurn(TurnOf(ReferenceAngles::FromTurn(turn)), turn));
            }
    }
}

// The sender's tick rides on every move (2026-11-18), so the receiver shows a throw at the speed and along the arc the
// sender had it. It must come out the same on the other side, and an old-style message without one is a plain 0 default.
TEST_CASE("a move carries the sender's tick through every hop", "[droppeditems]")
{
    constexpr uint64_t cTick = 123456789012ull;
    for (const bool atRest : {false, true})
    {
        RequestDroppedItemMove request{};
        request.Id = 5;
        request.Position = glm::vec3(100.f, 200.f, 300.f);
        request.Rotation = glm::vec3(0.1f, 0.2f, 0.3f);
        request.AtRest = atRest;
        request.Tick = cTick;
        Buffer buffer(256);
        Buffer::Writer writer(&buffer);
        request.SerializeRaw(writer);
        Buffer::Reader reader(&buffer);
        RequestDroppedItemMove back{};
        back.DeserializeRaw(reader);
        REQUIRE(back.Tick == cTick);
        REQUIRE(back.AtRest == atRest);
        REQUIRE(back == request);

        NotifyDroppedItemMove notify{};
        notify.Id = 5;
        notify.Position = request.Position;
        notify.Rotation = request.Rotation;
        notify.AtRest = atRest;
        notify.Tick = back.Tick;
        Buffer buffer2(256);
        Buffer::Writer writer2(&buffer2);
        notify.SerializeRaw(writer2);
        Buffer::Reader reader2(&buffer2);
        NotifyDroppedItemMove back2{};
        back2.DeserializeRaw(reader2);
        REQUIRE(back2.Tick == cTick);
        REQUIRE(back2 == notify);
    }

    RequestWorldObjectMove object{};
    object.Position = glm::vec3(-50.f, 70.f, 10.f);
    object.Rotation = glm::vec3(1.f, 2.f, 3.f);
    object.AtRest = true;
    object.Taken = false;
    object.Tick = cTick;
    Buffer buffer3(256);
    Buffer::Writer writer3(&buffer3);
    object.SerializeRaw(writer3);
    Buffer::Reader reader3(&buffer3);
    RequestWorldObjectMove back3{};
    back3.DeserializeRaw(reader3);
    REQUIRE(back3.Tick == cTick);
    REQUIRE(back3 == object);

    NotifyWorldObjectMove objectNotify{};
    objectNotify.Position = object.Position;
    objectNotify.Rotation = object.Rotation;
    objectNotify.AtRest = true;
    objectNotify.Tick = back3.Tick;
    Buffer buffer4(256);
    Buffer::Writer writer4(&buffer4);
    objectNotify.SerializeRaw(writer4);
    Buffer::Reader reader4(&buffer4);
    NotifyWorldObjectMove back4{};
    back4.DeserializeRaw(reader4);
    REQUIRE(back4.Tick == cTick);
    REQUIRE(back4 == objectNotify);

    // A message with no tick keeps the old meaning: 0.
    REQUIRE(RequestDroppedItemMove{}.Tick == 0);
    REQUIRE(NotifyWorldObjectMove{}.Tick == 0);
}

namespace
{
template <class T> T RoundTrip(const T& acMessage)
{
    Buffer buffer(256);
    Buffer::Writer writer(&buffer);
    acMessage.SerializeRaw(writer);
    Buffer::Reader reader(&buffer);
    T back{};
    back.DeserializeRaw(reader);
    return back;
}
} // namespace

// A thrown object's speed and spin ride on its moves from the let-go on, so the receiver's physics flies it on the same arc.
TEST_CASE("a thrown move carries its speed and spin through every hop", "[droppeditems]")
{
    const glm::vec3 cVelocity(350.f, -120.5f, 410.25f);
    const glm::vec3 cSpin(0.5f, -3.f, 12.f);

    RequestDroppedItemMove item{};
    item.Id = 9;
    item.Position = glm::vec3(1.f, 2.f, 3.f);
    item.Tick = 77;
    item.Thrown = true;
    item.Velocity = cVelocity;
    item.Spin = cSpin;
    const auto cItem = RoundTrip(item);
    REQUIRE(cItem == item);
    REQUIRE(cItem.Thrown);
    REQUIRE(cItem.Velocity == cVelocity);
    REQUIRE(cItem.Spin == cSpin);

    NotifyDroppedItemMove itemNotify{};
    itemNotify.Id = 9;
    itemNotify.Tick = 77;
    itemNotify.MoverPlayerId = 4;
    itemNotify.Thrown = cItem.Thrown;
    itemNotify.Velocity = cItem.Velocity;
    itemNotify.Spin = cItem.Spin;
    const auto cItemNotify = RoundTrip(itemNotify);
    REQUIRE(cItemNotify == itemNotify);
    REQUIRE(cItemNotify.MoverPlayerId == 4);

    RequestWorldObjectMove object{};
    object.ObjectId = GameId(1, 0x1234);
    object.CellId = GameId(0, 0x3C);
    object.Position = glm::vec3(-50.f, 70.f, 10.f);
    object.Thrown = true;
    object.Velocity = cVelocity;
    object.Spin = cSpin;
    REQUIRE(RoundTrip(object) == object);

    NotifyWorldObjectMove objectNotify{};
    objectNotify.ObjectId = object.ObjectId;
    objectNotify.Thrown = true;
    objectNotify.Velocity = cVelocity;
    objectNotify.Spin = cSpin;
    objectNotify.MoverPlayerId = 2;
    REQUIRE(RoundTrip(objectNotify) == objectNotify);
}

TEST_CASE("a move not thrown sends no speed and reads none back", "[droppeditems]")
{
    RequestDroppedItemMove item{};
    item.Id = 3;
    // Not thrown: what was set is not sent, so it reads back as none.
    item.Velocity = glm::vec3(5.f, 6.f, 7.f);
    item.Spin = glm::vec3(1.f, 0.f, 0.f);
    Buffer thrownBuffer(256);
    Buffer::Writer thrownWriter(&thrownBuffer);
    RequestDroppedItemMove thrown = item;
    thrown.Thrown = true;
    thrown.SerializeRaw(thrownWriter);
    Buffer plainBuffer(256);
    Buffer::Writer plainWriter(&plainBuffer);
    item.SerializeRaw(plainWriter);
    REQUIRE(thrownWriter.GetBitPosition() >= plainWriter.GetBitPosition() + 6 * 32);

    const auto cBack = RoundTrip(item);
    REQUIRE_FALSE(cBack.Thrown);
    REQUIRE(cBack.Velocity == glm::vec3(0.f));
    REQUIRE(cBack.Spin == glm::vec3(0.f));

    REQUIRE_FALSE(NotifyWorldObjectMove{}.Thrown);
    REQUIRE_FALSE(RoundTrip(NotifyWorldObjectMove{}).Thrown);
}

TEST_CASE("A reannounced drop matches the saved stack, including its extras", "[droppeditems]")
{
    Inventory::Entry saved = EnchantedDagger();
    saved.Count = 1;
    Inventory::Entry announced = saved;
    announced.Count = -saved.Count;
    CHECK(DroppedItemIdentity::SameStack(saved, announced));
    saved.ExtraWorn = true;
    CHECK(DroppedItemIdentity::SameStack(saved, announced));

    announced.Count = -2;
    CHECK_FALSE(DroppedItemIdentity::SameStack(saved, announced));
    announced.Count = -saved.Count;
    announced.ExtraHealth += 0.1f;
    CHECK_FALSE(DroppedItemIdentity::SameStack(saved, announced));
    announced.ExtraHealth = saved.ExtraHealth;
    announced.Data.DisplayName = "Renamed dagger";
    CHECK_FALSE(DroppedItemIdentity::SameStack(saved, announced));
    announced.Data.DisplayName.clear();
    announced.EnchantData.Effects.push_back({10.f, 0, 1, 0.f, GameId(0, 0x123)});
    CHECK_FALSE(DroppedItemIdentity::SameStack(saved, announced));
}

TEST_CASE("Same-base pending drops require their own reply and complete stack identity", "[droppeditems]")
{
    const Inventory::Entry first = EnchantedDagger();
    Inventory::Entry second = first;
    second.Data.DisplayName = "Second dagger";
    CHECK(DroppedItemIdentity::IsReply(12, second, 12, second));
    CHECK_FALSE(DroppedItemIdentity::IsReply(11, first, 12, second));
    CHECK_FALSE(DroppedItemIdentity::IsReply(11, first, 0, first));
    CHECK_FALSE(DroppedItemIdentity::IsReply(0, first, 0, first));
    CHECK_FALSE(DroppedItemIdentity::IsReply(11, first, 11, second));
    second = first;
    second.ExtraHealth += 0.1f;
    CHECK_FALSE(DroppedItemIdentity::IsReply(11, first, 11, second));
    second = first;
    second.Count = -2;
    CHECK_FALSE(DroppedItemIdentity::IsReply(11, first, 11, second));
    second = first;
    second.Count = 1;
    CHECK(DroppedItemIdentity::IsReply(11, first, 11, second));
}

TEST_CASE("Nearby adoption distinguishes name temper and count at the same position", "[droppeditems]")
{
    Inventory::Entry wanted = EnchantedDagger();
    wanted.Count = 1;
    wanted.Data.DisplayName = "My dagger";
    DroppedItemIdentity::Nearby nearby(wanted, false, 200.f, true);
    Inventory::Entry other = wanted;
    other.Data.DisplayName = "Their dagger";
    nearby.Consider(1, other, 0.f);
    other = wanted;
    other.ExtraHealth += 0.1f;
    nearby.Consider(2, other, 0.f);
    other = wanted;
    other.Count = 2;
    nearby.Consider(3, other, 0.f);
    nearby.Consider(4, wanted, 201.f);
    CHECK(nearby.Get().Id == 0);
    nearby.Consider(5, wanted, 20.f);
    CHECK(nearby.Get().Id == 5);
    CHECK_FALSE(nearby.Get().Ambiguous);
}

TEST_CASE("Simultaneous same-stack fresh adds place the peer copy without adopting the pending reference", "[droppeditems]")
{
    DroppedItemIdentity::Pending pending{};
    pending.RefFormId = 7;
    pending.RequestId = 11;
    pending.Item = EnchantedDagger();
    pending.CellId = GameId(0, 0xabc);
    auto peer = pending.Item;
    peer.Count = 1;
    CHECK_FALSE(DroppedItemIdentity::IsReply(pending.RequestId, pending.Item, 0, peer));
    CHECK_FALSE(DroppedItemIdentity::WaitingForReplay(pending, peer, false, false, pending.CellId, {}, {}, 200.f));
    CHECK_FALSE(DroppedItemIdentity::WaitingForReplay(pending, peer, false, true, pending.CellId, {}, {}, 200.f));
    DroppedItemIdentity::Nearby nearby(peer, false, 200.f, false);
    nearby.Consider(pending.RefFormId, pending.Item, 0.f, true);
    CHECK(nearby.Get().Id == 0); // No adoptable copy: the peer item is placed even at the same position.
    CHECK_FALSE(nearby.Get().Ambiguous);
    CHECK(DroppedItemIdentity::IsReply(pending.RequestId, pending.Item, 11, peer));
}

TEST_CASE("Spatially scoped modern and legacy replays wait for an in-flight reannouncement", "[droppeditems]")
{
    DroppedItemIdentity::Pending pending{};
    pending.Item = EnchantedDagger();
    pending.CellId = GameId(0, 0xabc);
    pending.Announcement = true;
    for (bool legacy : {false, true})
    {
        CHECK(DroppedItemIdentity::WaitingForReplay(pending, pending.Item, true, legacy, pending.CellId, {}, {}, 200.f));
        CHECK_FALSE(DroppedItemIdentity::WaitingForReplay(pending, pending.Item, false, legacy, pending.CellId, {}, {}, 200.f));
        CHECK_FALSE(DroppedItemIdentity::WaitingForReplay(pending, pending.Item, true, legacy, GameId(0, 0xdef), {}, {}, 200.f));
        CHECK_FALSE(DroppedItemIdentity::WaitingForReplay(pending, pending.Item, true, legacy, pending.CellId, {}, glm::vec3(201.f, 0.f, 0.f), 200.f));
    }
    auto unequal = pending.Item;
    unequal.Count = 2;
    CHECK_FALSE(DroppedItemIdentity::WaitingForReplay(pending, unequal, true, true, pending.CellId, {}, {}, 200.f));
}

TEST_CASE("A modern replay before its add reply waits without placing another floor copy", "[droppeditems]")
{
    DroppedItemIdentity::Pending pending{};
    pending.Item = EnchantedDagger();
    pending.Item.Data.DisplayName = "Saved dagger";
    pending.CellId = GameId(0, 0xabc);
    pending.RequestId = 17;
    pending.Announcement = true;
    auto replayed = pending.Item;
    replayed.Count = 1;
    CHECK(DroppedItemIdentity::WaitingForReplay(pending, replayed, true, false, pending.CellId, {}, {}, 200.f));
    CHECK_FALSE(DroppedItemIdentity::IsReply(pending.RequestId, pending.Item, 0, replayed));
    CHECK(DroppedItemIdentity::IsReply(pending.RequestId, pending.Item, 17, replayed));

    // A replay with no pending announcement and no floor copy still proceeds to placement.
    DroppedItemIdentity::Nearby empty(replayed, false, 200.f, true);
    CHECK(empty.Get().Id == 0);
    CHECK_FALSE(empty.Get().Ambiguous);
}

TEST_CASE("A fresh peer drop cannot adopt an unrelated identical floor stack", "[droppeditems]")
{
    const auto item = EnchantedDagger();
    DroppedItemIdentity::Nearby fresh(item, false, 200.f, false);
    fresh.Consider(7, item, 0.f); // Equal extras and location cannot prove a fresh peer drop is this saved copy.
    CHECK(fresh.Get().Id == 0);
    CHECK_FALSE(fresh.Get().Ambiguous);
    DroppedItemIdentity::Nearby replay(item, false, 200.f, true);
    replay.Consider(7, item, 0.f);
    CHECK(replay.Get().Id == 7);
}

TEST_CASE("An offline cached drop must still be the stack lying under its reference id", "[droppeditems]")
{
    const auto cached = EnchantedDagger();
    auto floor = cached;
    floor.Count = 1;
    floor.ExtraWorn = false;
    CHECK(DroppedItemIdentity::SameStack(cached, floor));
    floor.Data.DisplayName = "Another stack under the old ref id";
    CHECK_FALSE(DroppedItemIdentity::SameStack(cached, floor));
    floor = cached;
    floor.Count = 2;
    CHECK_FALSE(DroppedItemIdentity::SameStack(cached, floor));
    floor = cached;
    floor.ExtraHealth += 0.1f;
    CHECK_FALSE(DroppedItemIdentity::SameStack(cached, floor));
}

TEST_CASE("Dropped-item notifications distinguish fresh additions from replay on the wire", "[droppeditems]")
{
    for (bool replay : {false, true})
    {
        NotifyDroppedItem sent{};
        sent.Id = 42;
        sent.Item = EnchantedDagger();
        sent.Replay = replay;
        Buffer buffer(4096);
        Buffer::Writer writer(&buffer);
        sent.SerializeRaw(writer);
        Buffer::Reader reader(&buffer);
        NotifyDroppedItem received{};
        received.DeserializeRaw(reader);
        CHECK(received == sent);
        CHECK(received.Replay == replay);
    }
}

TEST_CASE("Saved drop matching uses scope nearest complete identity and legacy provenance", "[droppeditems]")
{
    DroppedItemRecord item{};
    item.Id = 1;
    item.Entry = EnchantedDagger();
    item.Entry.Count = 1;
    item.CellId = GameId(0, 0xabc);
    std::map<uint32_t, DroppedItemRecord> items{{1, item}};
    Inventory::Entry announced = item.Entry;
    announced.Data.DisplayName = "Old saved name";
    const auto find = [&]
    {
        return DroppedItemIdentity::Find(items, announced, item.CellId, {}, item.Position, 200.f);
    };
    CHECK(find().Id == 0); // An intentionally unnamed v3/v4 item is not a wildcard.
    announced = item.Entry;
    announced.ExtraHealth += 0.1f;
    CHECK(find().Id == 0);
    announced = item.Entry;
    announced.Count = 2;
    CHECK(find().Id == 0);
    announced = item.Entry;
    announced.Data.DisplayName = "Old saved name";
    items.at(1).LegacyItemData = true;
    CHECK(find().Id == 1);
    items.at(1).CellId = GameId(0, 0xdef);
    CHECK(find().Id == 0);
    items.at(1).CellId = item.CellId;
    items.at(1).Position = glm::vec3(201.f, 0.f, 0.f);
    CHECK(find().Id == 0);
    items.at(1).Position = item.Position;
    items.emplace(2, items.at(1));
    items.at(2).Id = 2;
    CHECK(find().Ambiguous);
    CHECK(find().Id == 0);
    items.at(2).LegacyItemData = false;
    items.at(2).Entry = announced;
    CHECK(find().Id == 2); // A complete exact record takes precedence over uncertain legacy data.
    items.emplace(3, items.at(2));
    items.at(3).Id = 3;
    items.at(2).Position = glm::vec3(30.f, 0.f, 0.f);
    CHECK(find().Id == 3);
    items.at(3).WorldSpaceId = GameId(0, 0x123);
    CHECK(find().Id == 2);
}

TEST_CASE("Old drop ledgers preserve missing metadata through a new save and reconnect", "[droppeditems]")
{
    for (uint64_t version : {1, 2, 3})
    {
        INFO("ledger version " << version);
        DroppedItemRecord old{};
        old.Id = 42;
        old.Entry = EnchantedDagger();
        old.Entry.Count = 1;
        old.CellId = GameId(0, 0xabc);
        old.Position = glm::vec3(10.f, 20.f, 30.f);
        old.Rotation = glm::vec3(0.f, 1.f, 2.f);
        if (version == 3)
            old.Entry.Data.DisplayName = "Known name";
        Buffer buffer(4096);
        Buffer::Writer writer(&buffer);
        WriteOldRecord(writer, old, version);
        Serialization::WriteVarInt(writer, 99);
        Buffer::Reader reader(&buffer);
        DroppedItemRecord loaded{};
        loaded.Deserialize(reader, version);
        REQUIRE(Serialization::ReadVarInt(reader) == 99);
        CHECK(loaded.Id == 42);
        CHECK(loaded.Entry == old.Entry);
        CHECK(loaded.CellId == old.CellId);
        CHECK(loaded.Position == old.Position);
        CHECK(loaded.Rotation == (version == 1 ? glm::vec3{} : old.Rotation));
        CHECK(loaded.LegacyItemData == (version < 3));

        Buffer saved(4096);
        Buffer::Writer savedWriter(&saved);
        loaded.Serialize(savedWriter);
        Serialization::WriteVarInt(savedWriter, 99);
        Buffer::Reader savedReader(&saved);
        DroppedItemRecord reconnected{};
        reconnected.Deserialize(savedReader, DroppedItemRecord::kVersion);
        CHECK(Serialization::ReadVarInt(savedReader) == 99);
        CHECK(reconnected.LegacyItemData == loaded.LegacyItemData);
        CHECK(reconnected.Entry == loaded.Entry);
        CHECK(reconnected.Id == loaded.Id);

        NotifyDroppedItem replay{};
        replay.Id = reconnected.Id;
        replay.Item = reconnected.Entry;
        replay.CellId = reconnected.CellId;
        replay.LegacyItemData = reconnected.LegacyItemData;
        replay.Replay = true;
        Buffer wire(4096);
        Buffer::Writer wireWriter(&wire);
        replay.SerializeRaw(wireWriter);
        Buffer::Reader wireReader(&wire);
        NotifyDroppedItem received{};
        received.DeserializeRaw(wireReader);
        REQUIRE(received == replay);

        // A fresh client with no floor copy still proceeds to placement using the recorded entry.
        DroppedItemIdentity::Nearby fresh(received.Item, received.LegacyItemData, 200.f, received.Replay);
        CHECK(fresh.Get().Id == 0);
        CHECK_FALSE(fresh.Get().Ambiguous);
        if (version >= 3)
            continue;

        Inventory::Entry floor = received.Item;
        floor.Data.DisplayName = "Recovered name";
        DroppedItemIdentity::Nearby nearby(received.Item, received.LegacyItemData, 200.f, received.Replay);
        nearby.Consider(7, floor, 0.f);
        nearby.Consider(7, floor, 0.f); // The same cell may be scanned twice.
        REQUIRE(nearby.Get().Id == 7);
        nearby.Consider(8, floor, 201.f);
        CHECK_FALSE(nearby.Get().Ambiguous);

        std::map<uint32_t, DroppedItemRecord> items{{reconnected.Id, reconnected}};
        CHECK_FALSE(items.at(42).EnrichItemData(ItemData{}));
        CHECK(items.at(42).LegacyItemData); // A fresh client's plain fallback must not erase the unknown provenance.
        const auto match = DroppedItemIdentity::Find(items, floor, reconnected.CellId, {}, reconnected.Position, 200.f);
        REQUIRE(match.Id == 42);
        REQUIRE(items.at(match.Id).EnrichItemData(floor.Data));
        CHECK(DroppedItemIdentity::SameStack(items.at(42).Entry, floor));
        CHECK(items.size() == 1);
        CHECK_FALSE(items.at(42).EnrichItemData(ItemData{}));

        // Two indistinguishable old copies defer adoption instead of choosing the first or placing a third.
        nearby.Consider(9, floor, 0.f);
        CHECK(nearby.Get().Ambiguous);
        CHECK(nearby.Get().Id == 0);
    }
}

TEST_CASE("Moved placed objects' resting places survive a server restart with the dropped items", "[droppeditems]")
{
    DroppedItemRecord item{};
    item.Id = 7;
    item.Entry.BaseId = GameId(0, 0xF);
    item.Entry.Count = 25;
    item.CellId = GameId(0, 0x1A26F);
    item.Position = {10.f, 20.f, 30.f};

    PlacedObjectRecord cart{};
    cart.ObjectId = GameId(0, 0x3A1B2);
    cart.CellId = GameId(0, 0x9732);
    cart.WorldSpaceId = GameId(0, 0x3C);
    cart.Position = {1200.5f, -340.25f, 88.f};
    cart.Rotation = {0.f, 0.5f, -1.25f};

    SECTION("written and read back whole")
    {
        Buffer buffer(4096);
        Buffer::Writer writer(&buffer);
        DroppedItemFile::Write(writer, 8, std::vector<DroppedItemRecord>{item}, std::vector<PlacedObjectRecord>{cart});
        Buffer::Reader reader(&buffer);
        const auto cRead = DroppedItemFile::Read(reader);
        REQUIRE(cRead.Valid);
        CHECK(cRead.Version == DroppedItemRecord::kVersion);
        CHECK(cRead.NextId == 8);
        REQUIRE(cRead.Items.size() == 1);
        CHECK(cRead.Items[0].Entry == item.Entry);
        REQUIRE(cRead.Objects.size() == 1);
        CHECK(cRead.Objects[0].ObjectId == cart.ObjectId);
        CHECK(cRead.Objects[0].CellId == cart.CellId);
        CHECK(cRead.Objects[0].WorldSpaceId == cart.WorldSpaceId);
        // Kept at the network's precision, as the dropped items are.
        CHECK(glm::distance(static_cast<const glm::vec3&>(cRead.Objects[0].Position), static_cast<const glm::vec3&>(cart.Position)) < 1.f);
        CHECK(cRead.Objects[0].Rotation == cart.Rotation);
    }

    SECTION("a file from before placed objects were kept still loads its items")
    {
        Buffer buffer(4096);
        Buffer::Writer writer(&buffer);
        writer.WriteBits(DroppedItemFile::kMagic, 32);
        Serialization::WriteVarInt(writer, 4);
        Serialization::WriteVarInt(writer, 8);
        Serialization::WriteVarInt(writer, 1);
        item.Serialize(writer);
        Buffer::Reader reader(&buffer);
        const auto cRead = DroppedItemFile::Read(reader);
        REQUIRE(cRead.Valid);
        CHECK(cRead.Items.size() == 1);
        CHECK(cRead.Objects.empty());

        // As the file on disk: nothing after the last item.
        Buffer file(buffer.GetData(), writer.Size());
        Buffer::Reader fileReader(&file);
        const auto cFile = DroppedItemFile::Read(fileReader);
        REQUIRE(cFile.Valid);
        CHECK(cFile.NextId == 8);
        CHECK(cFile.Items.size() == 1);
        CHECK(cFile.Objects.empty());
    }

    SECTION("a server that reads only version 4 still reads the items of a file with places")
    {
        Buffer buffer(4096);
        Buffer::Writer writer(&buffer);
        DroppedItemFile::Write(writer, 8, std::vector<DroppedItemRecord>{item}, std::vector<PlacedObjectRecord>{cart});
        Buffer file(buffer.GetData(), writer.Size());
        Buffer::Reader reader(&file);

        // The older server's Load: versions above 4 are refused, the counted items read, and whatever follows ignored.
        uint64_t magic = 0;
        reader.ReadBits(magic, 32);
        const uint64_t cVersion = Serialization::ReadVarInt(reader);
        REQUIRE(magic == DroppedItemFile::kMagic);
        REQUIRE(cVersion >= 1);
        REQUIRE(cVersion <= 4);
        const uint64_t cNextId = Serialization::ReadVarInt(reader);
        const uint64_t cCount = Serialization::ReadVarInt(reader);
        std::vector<DroppedItemRecord> items;
        for (uint64_t i = 0; i < cCount && !reader.Eof(); ++i)
        {
            DroppedItemRecord read{};
            read.Deserialize(reader, cVersion);
            items.push_back(read);
        }
        CHECK(cNextId == 8);
        REQUIRE(items.size() == 1);
        CHECK(items[0].Id == item.Id);
        CHECK(items[0].Entry == item.Entry);
        CHECK(items[0].CellId == item.CellId);
    }

    SECTION("a trailer cut short loses the places but never the items")
    {
        Buffer itemsOnly(4096);
        Buffer::Writer itemsWriter(&itemsOnly);
        itemsWriter.WriteBits(DroppedItemFile::kMagic, 32);
        Serialization::WriteVarInt(itemsWriter, DroppedItemRecord::kVersion);
        Serialization::WriteVarInt(itemsWriter, 8);
        Serialization::WriteVarInt(itemsWriter, 1);
        item.Serialize(itemsWriter);
        const size_t cItemsEnd = itemsWriter.Size();

        Buffer buffer(4096);
        Buffer::Writer writer(&buffer);
        DroppedItemFile::Write(writer, 8, std::vector<DroppedItemRecord>{item}, std::vector<PlacedObjectRecord>{cart, cart});
        REQUIRE(writer.Size() > cItemsEnd);

        // The held-still trailer after the places, then the earlier-places trailer, both empty here: two tags and two one-byte counts each, 80 bits each.
        const size_t cPlacesEnd = writer.Size() - 20;
        for (size_t size = cItemsEnd; size < cPlacesEnd; ++size)
        {
            Buffer cut(buffer.GetData(), size);
            Buffer::Reader reader(&cut);
            const auto cRead = DroppedItemFile::Read(reader);
            REQUIRE(cRead.Valid);
            CHECK(cRead.Items.size() == 1);
            CHECK(cRead.Objects.empty());
        }
        // Cut in the held-still trailer: the places are whole and kept.
        for (size_t size = cPlacesEnd; size < writer.Size(); ++size)
        {
            Buffer cut(buffer.GetData(), size);
            Buffer::Reader reader(&cut);
            const auto cRead = DroppedItemFile::Read(reader);
            REQUIRE(cRead.Valid);
            CHECK(cRead.Items.size() == 1);
            CHECK(cRead.Objects.size() == 2);
            CHECK_FALSE(cRead.Items[0].Fixed);
        }

        Buffer whole(buffer.GetData(), writer.Size());
        Buffer::Reader reader(&whole);
        CHECK(DroppedItemFile::Read(reader).Objects.size() == 2);
    }

    SECTION("a trailer of a newer kind is skipped and the items kept")
    {
        Buffer buffer(4096);
        Buffer::Writer writer(&buffer);
        writer.WriteBits(DroppedItemFile::kMagic, 32);
        Serialization::WriteVarInt(writer, DroppedItemRecord::kVersion);
        Serialization::WriteVarInt(writer, 8);
        Serialization::WriteVarInt(writer, 1);
        item.Serialize(writer);
        writer.WriteBits(DroppedItemFile::kObjectsTag, 32);
        Serialization::WriteVarInt(writer, DroppedItemFile::kObjectsVersion + 1);
        Serialization::WriteVarInt(writer, 1);
        cart.Serialize(writer);
        writer.WriteBits(DroppedItemFile::kObjectsTag, 32);
        Buffer::Reader reader(&buffer);
        const auto cRead = DroppedItemFile::Read(reader);
        REQUIRE(cRead.Valid);
        CHECK(cRead.Items.size() == 1);
        CHECK(cRead.Objects.empty());
    }

    SECTION("a version 5 file, with the places straight after the items, still loads both")
    {
        Buffer buffer(4096);
        Buffer::Writer writer(&buffer);
        writer.WriteBits(DroppedItemFile::kMagic, 32);
        Serialization::WriteVarInt(writer, DroppedItemFile::kInlineObjectsVersion);
        Serialization::WriteVarInt(writer, 8);
        Serialization::WriteVarInt(writer, 1);
        item.Serialize(writer);
        Serialization::WriteVarInt(writer, 1);
        cart.Serialize(writer);
        Buffer file(buffer.GetData(), writer.Size());
        Buffer::Reader reader(&file);
        const auto cRead = DroppedItemFile::Read(reader);
        REQUIRE(cRead.Valid);
        REQUIRE(cRead.Items.size() == 1);
        CHECK(cRead.Items[0].Entry == item.Entry);
        REQUIRE(cRead.Objects.size() == 1);
        CHECK(cRead.Objects[0].ObjectId == cart.ObjectId);
    }

    SECTION("another file, or a newer version, is not read at all")
    {
        Buffer buffer(64);
        Buffer::Writer writer(&buffer);
        writer.WriteBits(DroppedItemFile::kMagic, 32);
        Serialization::WriteVarInt(writer, DroppedItemFile::kInlineObjectsVersion + 1);
        Buffer::Reader reader(&buffer);
        const auto cRead = DroppedItemFile::Read(reader);
        CHECK_FALSE(cRead.Valid);
        CHECK(cRead.Items.empty());
    }

    SECTION("no more places are kept than the file allows")
    {
        std::vector<PlacedObjectRecord> many(DroppedItemFile::kMaxObjects + 5, cart);
        Buffer buffer(static_cast<size_t>(many.size() * 64 + 64));
        Buffer::Writer writer(&buffer);
        DroppedItemFile::Write(writer, 1, std::vector<DroppedItemRecord>{}, many);
        Buffer::Reader reader(&buffer);
        CHECK(DroppedItemFile::Read(reader).Objects.size() == DroppedItemFile::kMaxObjects);
    }
}

TEST_CASE("Moves of a carried item or object name the player moving it", "[droppeditems]")
{
    NotifyDroppedItemMove notify{};
    notify.Id = 12;
    notify.Position = glm::vec3(10.f, 20.f, 30.f);
    notify.Rotation = glm::vec3(0.1f, 0.2f, 0.3f);
    notify.Tick = 123456789ull;
    notify.MoverPlayerId = 0xABCDEF01u;
    Buffer buffer(256);
    Buffer::Writer writer(&buffer);
    notify.SerializeRaw(writer);
    Buffer::Reader reader(&buffer);
    NotifyDroppedItemMove back{};
    back.DeserializeRaw(reader);
    REQUIRE(back.MoverPlayerId == 0xABCDEF01u);
    REQUIRE(back.Tick == notify.Tick);
    REQUIRE(back == notify);

    NotifyWorldObjectMove object{};
    object.ObjectId = GameId(3, 0x1234);
    object.CellId = GameId(0, 0x9);
    object.WorldSpaceId = GameId(0, 0x3C);
    object.Position = glm::vec3(-50.f, 70.f, 10.f);
    object.Rotation = glm::vec3(1.f, 2.f, 3.f);
    object.Tick = 77;
    object.MoverPlayerId = 5;
    Buffer buffer2(256);
    Buffer::Writer writer2(&buffer2);
    object.SerializeRaw(writer2);
    Buffer::Reader reader2(&buffer2);
    NotifyWorldObjectMove back2{};
    back2.DeserializeRaw(reader2);
    REQUIRE(back2.MoverPlayerId == 5u);
    REQUIRE(back2 == object);

    // A place sent again, not a move: nobody named, and the receiver goes by the nearest hand.
    REQUIRE(NotifyDroppedItemMove{}.MoverPlayerId == 0);
    REQUIRE(NotifyWorldObjectMove{}.MoverPlayerId == 0);
}

TEST_CASE("A weapon held still on a rack says so on every hop, and the server keeps it across a restart", "[droppeditems][sharedhomes]")
{
    SECTION("the put-down and its relay carry it, and a move does not")
    {
        for (const bool cFixed : {false, true})
        {
            RequestDroppedItemMove request{};
            request.Id = 12;
            request.Position = glm::vec3(10.f, 20.f, 30.f);
            request.AtRest = true;
            request.Fixed = cFixed;
            Buffer buffer(256);
            Buffer::Writer writer(&buffer);
            request.SerializeRaw(writer);
            Buffer::Reader reader(&buffer);
            RequestDroppedItemMove back{};
            back.DeserializeRaw(reader);
            CHECK(back == request);

            NotifyDroppedItemMove notify{};
            notify.Id = 12;
            notify.AtRest = true;
            notify.Fixed = cFixed;
            Buffer buffer2(256);
            Buffer::Writer writer2(&buffer2);
            notify.SerializeRaw(writer2);
            Buffer::Reader reader2(&buffer2);
            NotifyDroppedItemMove back2{};
            back2.DeserializeRaw(reader2);
            CHECK(back2 == notify);
        }
    }

    SECTION("a player who comes later places it held still")
    {
        NotifyDroppedItem sent{};
        sent.Id = 4;
        sent.Item.BaseId = GameId(0, 0x12EB7); // Iron Sword
        sent.Item.Count = 1;
        sent.Position = glm::vec3(1.f, 2.f, 3.f);
        sent.Fixed = true;
        Buffer buffer(512);
        Buffer::Writer writer(&buffer);
        sent.SerializeRaw(writer);
        Buffer::Reader reader(&buffer);
        NotifyDroppedItem back{};
        back.DeserializeRaw(reader);
        CHECK(back.Fixed);
        CHECK(back == sent);
    }

    DroppedItemRecord racked{};
    racked.Id = 3;
    racked.Entry.BaseId = GameId(0, 0x12EB7);
    racked.Entry.Count = 1;
    racked.Fixed = true;
    DroppedItemRecord loose = racked;
    loose.Id = 9;
    loose.Fixed = false;

    SECTION("written and read back, only the racked one held still")
    {
        Buffer buffer(4096);
        Buffer::Writer writer(&buffer);
        DroppedItemFile::Write(writer, 10, std::vector<DroppedItemRecord>{racked, loose}, std::vector<PlacedObjectRecord>{});
        Buffer::Reader reader(&buffer);
        const auto cRead = DroppedItemFile::Read(reader);
        REQUIRE(cRead.Valid);
        // Still version 4: an older server still reads the items, and stops before the trailers.
        CHECK(cRead.Version == 4);
        REQUIRE(cRead.Items.size() == 2);
        CHECK(cRead.Items[0].Fixed);
        CHECK_FALSE(cRead.Items[1].Fixed);
    }

    SECTION("a server that reads only version 4 loads the items of a file with the held-still trailer, and stops before it")
    {
        Buffer buffer(4096);
        Buffer::Writer writer(&buffer);
        DroppedItemFile::Write(writer, 10, std::vector<DroppedItemRecord>{racked, loose}, std::vector<PlacedObjectRecord>{});
        Buffer file(buffer.GetData(), writer.Size());
        Buffer::Reader reader(&file);

        // The older server's Load, as in the placed objects' case: the counted items are read, whatever follows is ignored.
        uint64_t magic = 0;
        reader.ReadBits(magic, 32);
        REQUIRE(magic == DroppedItemFile::kMagic);
        REQUIRE(Serialization::ReadVarInt(reader) == 4);
        CHECK(Serialization::ReadVarInt(reader) == 10);
        REQUIRE(Serialization::ReadVarInt(reader) == 2);
        for (const uint32_t cId : {3u, 9u})
        {
            DroppedItemRecord item{};
            item.Deserialize(reader, 4);
            CHECK(item.Id == cId);
            CHECK(item.Entry == racked.Entry);
            CHECK_FALSE(item.Fixed);
        }
    }

    SECTION("a file from before, or one cut short in its last trailer, holds nothing still")
    {
        Buffer buffer(4096);
        Buffer::Writer writer(&buffer);
        DroppedItemFile::Write(writer, 10, std::vector<DroppedItemRecord>{racked, loose}, std::vector<PlacedObjectRecord>{});
        // The earlier-places trailer follows, empty here: two tags and two one-byte counts.
        const size_t cWhole = writer.Size() - 10;
        // Without the closing tag, then without the whole trailer: the items still load, none held still.
        for (const size_t cCut : {cWhole - 4, cWhole - 4 - 4 - 1 - 1 - 1})
        {
            Buffer file(buffer.GetData(), cCut);
            Buffer::Reader reader(&file);
            const auto cRead = DroppedItemFile::Read(reader);
            REQUIRE(cRead.Valid);
            REQUIRE(cRead.Items.size() == 2);
            CHECK_FALSE(cRead.Items[0].Fixed);
            CHECK_FALSE(cRead.Items[1].Fixed);
        }
    }
}

namespace
{
Vector3_NetQuantize Place(const float aX, const float aY, const float aZ)
{
    Vector3_NetQuantize place{};
    place = glm::vec3(aX, aY, aZ);
    return place;
}

float Off(const Vector3_NetQuantize& acPlace, const float aX, const float aY, const float aZ)
{
    return glm::distance(static_cast<const glm::vec3&>(acPlace), glm::vec3(aX, aY, aZ));
}
} // namespace

TEST_CASE("Moved item: a dropped item keeps where it rested before it was moved far", "[droppeditems]")
{
    DroppedItemRecord item{};
    item.Position = Place(0.f, 0.f, 0.f);
    item.RestPosition = item.Position;

    SECTION("a small move keeps no place, and nudges add up from the old resting place")
    {
        item.NoteRest(glm::vec3(150.f, 0.f, 0.f), 200.f);
        CHECK(item.EarlierPlaces.empty());
        item.NoteRest(glm::vec3(190.f, 0.f, 0.f), 200.f);
        CHECK(item.EarlierPlaces.empty());
        item.NoteRest(glm::vec3(300.f, 0.f, 0.f), 200.f);
        REQUIRE(item.EarlierPlaces.size() == 1);
        CHECK(Off(item.EarlierPlaces[0], 0.f, 0.f, 0.f) < 1.f);
    }

    SECTION("a far move keeps the old resting place, and a place already kept is not kept twice")
    {
        item.NoteRest(glm::vec3(1000.f, 0.f, 0.f), 200.f);
        item.NoteRest(glm::vec3(50.f, 0.f, 0.f), 200.f); // back near where it was dropped
        item.NoteRest(glm::vec3(2000.f, 0.f, 0.f), 200.f);
        REQUIRE(item.EarlierPlaces.size() == 2);
        CHECK(Off(item.EarlierPlaces[0], 0.f, 0.f, 0.f) < 1.f);
        CHECK(Off(item.EarlierPlaces[1], 1000.f, 0.f, 0.f) < 1.f);
    }

    SECTION("the place it was dropped is never forgotten, the oldest other is")
    {
        for (int i = 1; i <= 6; ++i)
            item.NoteRest(glm::vec3(1000.f * static_cast<float>(i), 0.f, 0.f), 200.f);
        REQUIRE(item.EarlierPlaces.size() == DroppedItemRecord::kMaxEarlierPlaces);
        CHECK(Off(item.EarlierPlaces[0], 0.f, 0.f, 0.f) < 1.f);
        CHECK(Off(item.EarlierPlaces[1], 3000.f, 0.f, 0.f) < 1.f);
        CHECK(Off(item.EarlierPlaces.back(), 5000.f, 0.f, 0.f) < 1.f);
    }

    SECTION("an announcement from a save that still has it where it rested matches the item, not a new one")
    {
        item.Id = 5;
        item.Entry.BaseId = GameId(0, 0xF);
        item.Entry.Count = 3;
        item.CellId = GameId(0, 0x1A26F);
        item.NoteRest(glm::vec3(900.f, 0.f, 0.f), 200.f);
        item.Position = Place(900.f, 0.f, 0.f);
        std::map<uint32_t, DroppedItemRecord> items{{5, item}};
        CHECK(DroppedItemIdentity::Find(items, item.Entry, item.CellId, {}, Place(30.f, 0.f, 0.f), 200.f).Id == 5);
        CHECK(DroppedItemIdentity::Find(items, item.Entry, item.CellId, {}, Place(500.f, 0.f, 0.f), 200.f).Id == 0);
    }
}

TEST_CASE("Moved item: earlier places travel with the item and survive a server restart", "[droppeditems]")
{
    DroppedItemRecord item{};
    item.Id = 7;
    item.Entry.BaseId = GameId(0, 0xF);
    item.Entry.Count = 25;
    item.CellId = GameId(0, 0x1A26F);
    item.Position = Place(2000.f, 0.f, 0.f);
    item.EarlierPlaces = {Place(10.f, 20.f, 30.f), Place(900.f, -40.f, 5.f)};
    DroppedItemRecord plain = item;
    plain.Id = 8;
    plain.EarlierPlaces.clear();

    SECTION("the message carries them, and a list too long is cut on reading")
    {
        NotifyDroppedItem notify{};
        notify.Id = 7;
        notify.Item = item.Entry;
        notify.EarlierPlaces.assign(item.EarlierPlaces.begin(), item.EarlierPlaces.end());
        Buffer buffer(1024);
        Buffer::Writer writer(&buffer);
        notify.SerializeRaw(writer);
        Buffer::Reader reader(&buffer);
        NotifyDroppedItem back{};
        back.DeserializeRaw(reader);
        CHECK(back == notify);
        REQUIRE(back.EarlierPlaces.size() == 2);

        // The sender writes no more than the cap.
        for (int i = 0; i < 20; ++i)
            notify.EarlierPlaces.push_back(Place(static_cast<float>(i), 0.f, 0.f));
        Buffer longBuffer(2048);
        Buffer::Writer longWriter(&longBuffer);
        notify.SerializeRaw(longWriter);
        Buffer::Reader longReader(&longBuffer);
        NotifyDroppedItem longBack{};
        DecodeGuard::t_failed = false;
        longBack.DeserializeRaw(longReader);
        CHECK_FALSE(DecodeGuard::t_failed);
        CHECK(longBack.EarlierPlaces.size() == NotifyDroppedItem::kMaxEarlierPlaces);

        // A count over the cap, as a damaged or crafted message has it, fails the whole message.
        Buffer crafted(1024);
        Buffer::Writer craftedWriter(&crafted);
        Serialization::WriteVarInt(craftedWriter, 7);
        notify.Item.Serialize(craftedWriter);
        notify.CellId.Serialize(craftedWriter);
        notify.WorldSpaceId.Serialize(craftedWriter);
        notify.Position.Serialize(craftedWriter);
        Serialization::WriteFloat(craftedWriter, 0.f);
        Serialization::WriteFloat(craftedWriter, 0.f);
        Serialization::WriteFloat(craftedWriter, 0.f);
        Serialization::WriteVarInt(craftedWriter, 0);
        Serialization::WriteBool(craftedWriter, false);
        Serialization::WriteBool(craftedWriter, true);
        Serialization::WriteBool(craftedWriter, false);
        Serialization::WriteVarInt(craftedWriter, NotifyDroppedItem::kMaxEarlierPlaces + 1);
        for (uint64_t i = 0; i <= NotifyDroppedItem::kMaxEarlierPlaces; ++i)
            Place(1.f, 2.f, 3.f).Serialize(craftedWriter);
        Buffer::Reader craftedReader(&crafted);
        NotifyDroppedItem craftedBack{};
        DecodeGuard::t_failed = false;
        craftedBack.DeserializeRaw(craftedReader);
        CHECK(DecodeGuard::t_failed);
        CHECK(craftedBack.EarlierPlaces.empty());
        DecodeGuard::t_failed = false;
    }

    SECTION("written and read back whole")
    {
        Buffer buffer(4096);
        Buffer::Writer writer(&buffer);
        DroppedItemFile::Write(writer, 9, std::vector<DroppedItemRecord>{item, plain}, std::vector<PlacedObjectRecord>{});
        Buffer file(buffer.GetData(), writer.Size());
        Buffer::Reader reader(&file);
        const auto cRead = DroppedItemFile::Read(reader);
        REQUIRE(cRead.Valid);
        REQUIRE(cRead.Items.size() == 2);
        REQUIRE(cRead.Items[0].EarlierPlaces.size() == 2);
        CHECK(Off(cRead.Items[0].EarlierPlaces[1], 900.f, -40.f, 5.f) < 1.f);
        CHECK(cRead.Items[1].EarlierPlaces.empty());
        // A loaded item rests where it lies.
        CHECK(Off(cRead.Items[0].RestPosition, 2000.f, 0.f, 0.f) < 1.f);
    }

    SECTION("a file written before earlier places were kept loads, with none")
    {
        // Version 4 with the objects' and held-still trailers, as the server wrote it before this change.
        Buffer buffer(4096);
        Buffer::Writer writer(&buffer);
        writer.WriteBits(DroppedItemFile::kMagic, 32);
        Serialization::WriteVarInt(writer, 4);
        Serialization::WriteVarInt(writer, 9);
        Serialization::WriteVarInt(writer, 1);
        plain.Serialize(writer);
        writer.WriteBits(DroppedItemFile::kObjectsTag, 32);
        Serialization::WriteVarInt(writer, DroppedItemFile::kObjectsVersion);
        Serialization::WriteVarInt(writer, 0);
        writer.WriteBits(DroppedItemFile::kObjectsTag, 32);
        writer.WriteBits(DroppedItemFile::kFixedTag, 32);
        Serialization::WriteVarInt(writer, DroppedItemFile::kFixedVersion);
        Serialization::WriteVarInt(writer, 1);
        Serialization::WriteVarInt(writer, plain.Id);
        writer.WriteBits(DroppedItemFile::kFixedTag, 32);
        Buffer file(buffer.GetData(), writer.Size());
        Buffer::Reader reader(&file);
        const auto cRead = DroppedItemFile::Read(reader);
        REQUIRE(cRead.Valid);
        REQUIRE(cRead.Items.size() == 1);
        CHECK(cRead.Items[0].Fixed);
        CHECK(cRead.Items[0].EarlierPlaces.empty());
        CHECK(Off(cRead.Items[0].RestPosition, 2000.f, 0.f, 0.f) < 1.f);
    }

    SECTION("an earlier-places trailer cut short is dropped and the rest kept, and a list too long is written capped")
    {
        Buffer buffer(4096);
        Buffer::Writer writer(&buffer);
        DroppedItemFile::Write(writer, 9, std::vector<DroppedItemRecord>{item}, std::vector<PlacedObjectRecord>{});
        const size_t cWhole = writer.Size();
        for (size_t size = cWhole - 8; size < cWhole; ++size)
        {
            Buffer cut(buffer.GetData(), size);
            Buffer::Reader reader(&cut);
            const auto cRead = DroppedItemFile::Read(reader);
            REQUIRE(cRead.Valid);
            REQUIRE(cRead.Items.size() == 1);
            CHECK(cRead.Items[0].EarlierPlaces.empty());
        }

        DroppedItemRecord tooMany = item;
        tooMany.EarlierPlaces.resize(DroppedItemRecord::kMaxEarlierPlaces + 3);
        Buffer longBuffer(4096);
        Buffer::Writer longWriter(&longBuffer);
        DroppedItemFile::Write(longWriter, 9, std::vector<DroppedItemRecord>{tooMany}, std::vector<PlacedObjectRecord>{});
        Buffer longFile(longBuffer.GetData(), longWriter.Size());
        Buffer::Reader longReader(&longFile);
        const auto cLong = DroppedItemFile::Read(longReader);
        REQUIRE(cLong.Items.size() == 1);
        CHECK(cLong.Items[0].EarlierPlaces.size() == DroppedItemRecord::kMaxEarlierPlaces);
    }
}

TEST_CASE("Moved item: at an earlier resting place only a single matching copy is taken", "[droppeditems]")
{
    Inventory::Entry stack{};
    stack.BaseId = GameId(0, 0xF);
    stack.Count = 3;
    Inventory::Entry otherCount = stack;
    otherCount.Count = 4;

    SECTION("one matching copy is taken")
    {
        DroppedItemIdentity::Nearby nearby(stack, false, 200.f, true, true);
        nearby.Consider(11, stack, 50.f);
        nearby.Consider(12, otherCount, 10.f); // another count is another stack
        const auto cMatch = nearby.Get();
        CHECK(cMatch.Id == 11);
        CHECK_FALSE(cMatch.Ambiguous);
    }

    SECTION("two matching copies, near or far, are ambiguous and none is taken")
    {
        DroppedItemIdentity::Nearby nearby(stack, false, 200.f, true, true);
        nearby.Consider(11, stack, 50.f);
        nearby.Consider(13, stack, 150.f); // farther than the first, still counted
        const auto cMatch = nearby.Get();
        CHECK(cMatch.Id == 0);
        CHECK(cMatch.Ambiguous);
    }

    SECTION("a world-placed look-alike there is neither taken nor makes it ambiguous")
    {
        DroppedItemIdentity::Nearby nearby(stack, false, 200.f, true, true);
        nearby.Consider(15, stack, 10.f, false, false); // a plugin's placed item, nearer
        nearby.Consider(11, stack, 50.f);               // this save's dropped copy
        const auto cMatch = nearby.Get();
        CHECK(cMatch.Id == 11);
        CHECK_FALSE(cMatch.Ambiguous);

        DroppedItemIdentity::Nearby onlyPlaced(stack, false, 200.f, true, true);
        onlyPlaced.Consider(15, stack, 10.f, false, false);
        CHECK(onlyPlaced.Get().Id == 0);
    }

    SECTION("where the item lies now, a world-placed copy is still considered as before")
    {
        DroppedItemIdentity::Nearby nearby(stack, false, 200.f, true);
        nearby.Consider(15, stack, 10.f, false, false);
        CHECK(nearby.Get().Id == 15);
    }

    SECTION("none there, or only one reserved for another item, takes none")
    {
        DroppedItemIdentity::Nearby nearby(stack, false, 200.f, true, true);
        nearby.Consider(11, stack, 250.f);      // beyond the radius
        nearby.Consider(14, stack, 20.f, true); // reserved
        const auto cMatch = nearby.Get();
        CHECK(cMatch.Id == 0);
        CHECK_FALSE(cMatch.Ambiguous);
    }

    SECTION("at the item's own place the nearest is still taken, as before")
    {
        DroppedItemIdentity::Nearby nearby(stack, false, 200.f, true);
        nearby.Consider(11, stack, 50.f);
        nearby.Consider(13, stack, 150.f);
        CHECK(nearby.Get().Id == 11);
    }
}

TEST_CASE("Moved item: a record lying at the announced place wins over one that only rested there before", "[droppeditems]")
{
    DroppedItemRecord moved{};
    moved.Id = 1;
    moved.Entry.BaseId = GameId(0, 0xF);
    moved.Entry.Count = 3;
    moved.CellId = GameId(0, 0x1A26F);
    moved.Position = Place(900.f, 0.f, 0.f);
    moved.EarlierPlaces = {Place(0.f, 0.f, 0.f)};
    DroppedItemRecord here = moved;
    here.Id = 2;
    here.Position = Place(60.f, 0.f, 0.f);
    here.EarlierPlaces.clear();

    std::map<uint32_t, DroppedItemRecord> items{{1, moved}, {2, here}};
    CHECK(DroppedItemIdentity::Find(items, moved.Entry, moved.CellId, {}, Place(10.f, 0.f, 0.f), 200.f).Id == 2);
    items.erase(2);
    CHECK(DroppedItemIdentity::Find(items, moved.Entry, moved.CellId, {}, Place(10.f, 0.f, 0.f), 200.f).Id == 1);
}

TEST_CASE("Moved item: an announcement still waiting for its reply is waited for at an earlier place too", "[droppeditems]")
{
    DroppedItemIdentity::Pending pending{};
    pending.RequestId = 4;
    pending.Announcement = true;
    pending.Item.BaseId = GameId(0, 0xF);
    pending.Item.Count = 3;
    pending.CellId = GameId(0, 0x1A26F);
    pending.Position = Place(20.f, 0.f, 0.f);
    // Far from where the item lies now, near where it rested before.
    CHECK_FALSE(DroppedItemIdentity::WaitingForReplay(pending, pending.Item, true, false, pending.CellId, {}, glm::vec3(900.f, 0.f, 0.f), 200.f));
    CHECK(DroppedItemIdentity::WaitingForReplay(pending, pending.Item, true, false, pending.CellId, {}, glm::vec3(0.f, 0.f, 0.f), 200.f));
}
