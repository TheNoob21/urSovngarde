#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/RequestQuestUpdate.h>
#include <Structs/VRPose.h>

#include <array>
#include <algorithm>

using TiltedPhoques::Buffer;

namespace
{
// Payloads only: message opcode framing is tested elsewhere. Constants follow TiltedCore v0.2.9:
// least-significant bit first and 7-bit varints with a continuation bit. WriteFloat aligns to a
// byte boundary; VRPose writes the raw float bits without alignment.
template <size_t N, typename T>
void CheckRawBytes(const T& acValue, const std::array<uint8_t, N>& acExpected)
{
    Buffer buffer(256);
    std::fill_n(buffer.GetWriteData(), buffer.GetSize(), uint8_t{0});
    Buffer::Writer writer(&buffer);
    acValue.SerializeRaw(writer);
    REQUIRE(writer.Size() == acExpected.size());
    CHECK(std::equal(acExpected.begin(), acExpected.end(), buffer.GetData()));
}

Inventory::Entry Item()
{
    Inventory::Entry item{};
    item.BaseId = GameId(4, 3);
    item.Count = 5;
    item.EnchantData.IsWeapon = true;
    item.ExtraWorn = true;
    item.IsQuestItem = true;
    return item;
}
}

TEST_CASE("Inventory payloads preserve their flag positions", "[wire.contract]")
{
    // ServerId=1, epoch=2, base/mod=3/4, count=5, empty extra data.
    // Last byte: five entry flags followed by request or notification flags.
    constexpr std::array<uint8_t, 22> requestBytes = {
        1, 2, 3, 4, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xb5};
    constexpr std::array<uint8_t, 22> notifyBytes = {
        1, 2, 3, 4, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x55};

    RequestInventoryChanges request;
    request.ServerId = 1;
    request.OwnershipEpoch = 2;
    request.Item = Item();
    request.Drop = true;
    request.UpdateClients = false;
    request.PickedUp = true;
    CheckRawBytes(request, requestBytes);

    Buffer requestBuffer(requestBytes.data(), requestBytes.size());
    Buffer::Reader requestReader(&requestBuffer);
    RequestInventoryChanges decodedRequest;
    decodedRequest.DeserializeRaw(requestReader);
    CHECK(decodedRequest.ServerId == 1);
    CHECK(decodedRequest.OwnershipEpoch == 2);
    CHECK(decodedRequest.Item.BaseId == GameId(4, 3));
    CHECK(decodedRequest.Item.Count == 5);
    CHECK(decodedRequest.Item.EnchantData.IsWeapon);
    CHECK(decodedRequest.Item.ExtraWorn);
    CHECK(decodedRequest.Item.IsQuestItem);
    CHECK_FALSE(decodedRequest.Item.ExtraWornLeft);
    CHECK(decodedRequest.Drop);
    CHECK_FALSE(decodedRequest.UpdateClients);
    CHECK(decodedRequest.PickedUp);

    NotifyInventoryChanges notify;
    notify.ServerId = 1;
    notify.OwnershipEpoch = 2;
    notify.Item = Item();
    notify.Drop = false;
    notify.PickedUp = true;
    CheckRawBytes(notify, notifyBytes);

    Buffer notifyBuffer(notifyBytes.data(), notifyBytes.size());
    Buffer::Reader notifyReader(&notifyBuffer);
    NotifyInventoryChanges decodedNotify;
    decodedNotify.DeserializeRaw(notifyReader);
    CHECK(decodedNotify.ServerId == 1);
    CHECK(decodedNotify.OwnershipEpoch == 2);
    CHECK(decodedNotify.Item.BaseId == GameId(4, 3));
    CHECK(decodedNotify.Item.Count == 5);
    CHECK(decodedNotify.Item.EnchantData.IsWeapon);
    CHECK(decodedNotify.Item.ExtraWorn);
    CHECK(decodedNotify.Item.IsQuestItem);
    CHECK_FALSE(decodedNotify.Drop);
    CHECK(decodedNotify.PickedUp);
}

TEST_CASE("Quest request payload has fixed field order", "[wire.contract]")
{
    // Base id 129 is the two-byte varint 81 01; stage is little-endian 16-bit.
    constexpr std::array<uint8_t, 7> bytes = {0x81, 0x01, 0x02, 0x34, 0x12, 0x01, 0x07};
    RequestQuestUpdate request;
    request.Id = GameId(2, 129);
    request.Stage = 0x1234;
    request.Status = 1;
    request.ClientQuestType = 7;
    CheckRawBytes(request, bytes);

    Buffer buffer(bytes.data(), bytes.size());
    Buffer::Reader reader(&buffer);
    RequestQuestUpdate decoded;
    decoded.DeserializeRaw(reader);
    CHECK(decoded.Id == GameId(2, 129));
    CHECK(decoded.Stage == 0x1234);
    CHECK(decoded.Status == 1);
    CHECK(decoded.ClientQuestType == 7);
}

TEST_CASE("Boneless VR pose has fixed scale and hip encoding", "[wire.contract]")
{
    // HasData, NoBones, no fingers, scale=1000, no root or hands, hips=(1,-2,0.5), no weapons.
    constexpr std::array<uint8_t, 15> bytes = {0x8b, 0x3e, 0x40, 0x00, 0x00, 0xc0, 0x1f, 0x00,
                                                0x00, 0x00, 0x60, 0x00, 0x00, 0x80, 0x1f};
    VRPose pose{};
    pose.HasData = true;
    pose.NoBones = true;
    pose.HasScale = true;
    pose.RootScale = 1000;
    pose.HasHips = true;
    pose.HipOffset[0] = 1.f;
    pose.HipOffset[1] = -2.f;
    pose.HipOffset[2] = 0.5f;
    Buffer out(256);
    std::fill_n(out.GetWriteData(), out.GetSize(), uint8_t{0});
    Buffer::Writer writer(&out);
    pose.Serialize(writer);
    REQUIRE(writer.Size() == bytes.size());
    CHECK(std::equal(bytes.begin(), bytes.end(), out.GetData()));

    Buffer input(bytes.data(), bytes.size());
    Buffer::Reader reader(&input);
    VRPose decoded{};
    decoded.Deserialize(reader);
    CHECK(decoded.HasData);
    CHECK(decoded.NoBones);
    CHECK_FALSE(decoded.HasFingers);
    CHECK(decoded.HasScale);
    CHECK(decoded.RootScale == 1000);
    CHECK(decoded.HasHips);
    CHECK(decoded.HipOffset[0] == 1.f);
    CHECK(decoded.HipOffset[1] == -2.f);
    CHECK(decoded.HipOffset[2] == 0.5f);
    CHECK_FALSE(decoded.HasRootPosition);
    CHECK_FALSE(decoded.HasHandCheck);
    CHECK_FALSE(decoded.HasWeapons);
}
