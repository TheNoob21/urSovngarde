#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <catch2/catch.hpp>

#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>

#include <array>
#include <type_traits>

using namespace TiltedPhoques;

// Every opcode's number on the wire. Upstream's keep upstream's numbers; ours follow upstream's last, in a fixed order.
// Moving one is a protocol change.

// Client opcodes.
static_assert(kAuthenticationRequest == 0);
static_assert(kAssignCharacterRequest == 1);
static_assert(kClientReferencesMoveRequest == 2);
static_assert(kEnterExteriorCellRequest == 3);
static_assert(kEnterInteriorCellRequest == 4);
static_assert(kRequestFactionsChanges == 5);
static_assert(kRequestQuestUpdate == 6);
static_assert(kPartyInviteRequest == 7);
static_assert(kPartyAcceptInviteRequest == 8);
static_assert(kPartyLeaveRequest == 9);
static_assert(kPartyCreateRequest == 10);
static_assert(kPartyChangeLeaderRequest == 11);
static_assert(kPartyKickRequest == 12);
static_assert(kRequestActorValueChanges == 13);
static_assert(kRequestActorMaxValueChanges == 14);
static_assert(kRequestHealthChangeBroadcast == 15);
static_assert(kActivateRequest == 16);
static_assert(kLockChangeRequest == 17);
static_assert(kAssignObjectsRequest == 18);
static_assert(kRequestDeathStateChange == 19);
static_assert(kShiftGridCellRequest == 20);
static_assert(kRequestOwnershipTransfer == 21);
static_assert(kRequestOwnershipClaim == 22);
static_assert(kRequestObjectInventoryChanges == 23);
static_assert(kRequestInventoryChanges == 24);
static_assert(kSpellCastRequest == 25);
static_assert(kInterruptCastRequest == 26);
static_assert(kAddTargetRequest == 27);
static_assert(kRequestRemoveSpell == 28);
static_assert(kProjectileLaunchRequest == 29);
static_assert(kScriptAnimationRequest == 30);
static_assert(kDrawWeaponRequest == 31);
static_assert(kMountRequest == 32);
static_assert(kNewPackageRequest == 33);
static_assert(kRequestRespawn == 34);
static_assert(kSyncExperienceRequest == 35);
static_assert(kRequestEquipmentChanges == 36);
static_assert(kSendChatMessageRequest == 37);
static_assert(kTeleportCommandRequest == 38);
static_assert(kPlayerRespawnRequest == 39);
static_assert(kDialogueRequest == 40);
static_assert(kSubtitleRequest == 41);
static_assert(kPlayerDialogueRequest == 42);
static_assert(kPlayerLevelRequest == 43);
static_assert(kTeleportRequest == 44);
static_assert(kRequestPlayerHealthUpdate == 45);
static_assert(kRequestWeatherChange == 46);
static_assert(kRequestCurrentWeather == 47);
static_assert(kRequestSetWaypoint == 48);
static_assert(kRequestRemoveWaypoint == 49);
static_assert(kSetTimeCommandRequest == 50);
static_assert(kRequestDroppedItemAdd == 51);
static_assert(kRequestDroppedItemRemove == 52);
static_assert(kRequestDroppedItemMove == 53);
static_assert(kRequestWorldObjectMove == 54);
static_assert(kClashRequest == 55);
static_assert(kPlayerPlaceRequest == 56);
static_assert(kVoiceDataRequest == 57);
static_assert(kVoiceStateRequest == 58);
static_assert(kArrowNockedRequest == 59);
static_assert(kPlayerDownedRequest == 60);
static_assert(kReviveRequest == 61);
static_assert(kSafePointRequest == 62);
static_assert(kRequestActionEvents == 63);
static_assert(kRequestHourMove == 64);
static_assert(kRequestReferenceEnable == 65);
static_assert(kRequestWarState == 66);
static_assert(kRequestFollowerState == 67);
static_assert(kRequestPartyPlugin == 68);
static_assert(kRequestPartyValue == 69);
static_assert(kRequestActorPresence == 70);
static_assert(kRequestNpcPlugins == 71);
static_assert(kRequestTimeSkip == 72);
static_assert(kRequestCombatState == 73);
// Ours, after upstream's last.
static_assert(kRequestQuestAliases == 74);
static_assert(kRequestHitReaction == 75);
static_assert(kRequestPartyFastTravel == 76);
static_assert(kRequestPartyFollow == 77);
static_assert(kRequestTrackedQuests == 78);
static_assert(kRequestDetectionLevels == 79);
static_assert(kRequestFollowerCommand == 80);
static_assert(kRequestCrimeReport == 81);
static_assert(kRequestStuckArrow == 82);
static_assert(kRequestDismember == 83);
static_assert(kRequestWordLearned == 84);
static_assert(kRequestTrapFire == 85);
static_assert(kRequestCombatTarget == 86);
static_assert(kRequestDismount == 87);
static_assert(kRequestNpcFurniture == 88);
static_assert(kRequestNpcHeadTrack == 89);
static_assert(kRequestRagdollPose == 90);
static_assert(kRequestPropBodies == 91);
static_assert(kRequestItemTake == 92);
static_assert(kRequestMapDiscovery == 93);
static_assert(kRequestItemDataChange == 94);
static_assert(kRequestMagicState == 95);
static_assert(kRequestPlayerFurniture == 96);
static_assert(kRequestExtension == 97);
static_assert(kRequestActivationState == 98);
static_assert(kRequestReferenceState == 99);
static_assert(kRequestQuestCatchUp == 100);
static_assert(kRequestArrowPickup == 101);
static_assert(kRequestSaveLoaded == 102);
static_assert(kClientOpcodeMax == 103);

// Server opcodes.
static_assert(kAuthenticationResponse == 0);
static_assert(kAssignCharacterResponse == 1);
static_assert(kServerReferencesMoveRequest == 2);
static_assert(kServerTimeSettings == 3);
static_assert(kCharacterSpawnRequest == 4);
static_assert(kNotifyFactionsChanges == 5);
static_assert(kNotifyRemoveCharacter == 6);
static_assert(kNotifyQuestUpdate == 7);
static_assert(kNotifyPlayerList == 8);
static_assert(kNotifyPartyInfo == 9);
static_assert(kNotifyPartyInvite == 10);
static_assert(kNotifyPartyJoined == 11);
static_assert(kNotifyPartyLeft == 12);
static_assert(kNotifyActorValueChanges == 13);
static_assert(kNotifyActorMaxValueChanges == 14);
static_assert(kNotifyHealthChangeBroadcast == 15);
static_assert(kNotifyActivate == 16);
static_assert(kNotifyLockChange == 17);
static_assert(kAssignObjectsResponse == 18);
static_assert(kNotifyDeathStateChange == 19);
static_assert(kNotifyOwnershipTransfer == 20);
static_assert(kNotifyObjectInventoryChanges == 21);
static_assert(kNotifyInventoryChanges == 22);
static_assert(kNotifySpellCast == 23);
static_assert(kNotifyInterruptCast == 24);
static_assert(kNotifyAddTarget == 25);
static_assert(kNotifyRemoveSpell == 26);
static_assert(kNotifyProjectileLaunch == 27);
static_assert(kNotifyScriptAnimation == 28);
static_assert(kNotifyDrawWeapon == 29);
static_assert(kNotifyMount == 30);
static_assert(kNotifyNewPackage == 31);
static_assert(kNotifyRespawn == 32);
static_assert(kNotifySyncExperience == 33);
static_assert(kNotifyEquipmentChanges == 34);
static_assert(kNotifyChatMessageBroadcast == 35);
static_assert(kTeleportCommandResponse == 36);
static_assert(kStringCacheUpdate == 37);
static_assert(kNotifyPlayerRespawn == 38);
static_assert(kNotifyDialogue == 39);
static_assert(kNotifySubtitle == 40);
static_assert(kNotifyPlayerDialogue == 41);
static_assert(kNotifyActorTeleport == 42);
static_assert(kNotifyPlayerLeft == 43);
static_assert(kNotifyPlayerJoined == 44);
static_assert(kNotifyPlayerLevel == 45);
static_assert(kNotifyPlayerCellChanged == 46);
static_assert(kNotifyTeleport == 47);
static_assert(kNotifyPlayerHealthUpdate == 48);
static_assert(kNotifySettingsChange == 49);
static_assert(kNotifyWeatherChange == 50);
static_assert(kNotifySetWaypoint == 51);
static_assert(kNotifyRemoveWaypoint == 52);
static_assert(kNotifySetTimeResult == 53);
static_assert(kNotifyDroppedItem == 54);
static_assert(kNotifyDroppedItemRemoved == 55);
static_assert(kNotifyDroppedItemMove == 56);
static_assert(kNotifyWorldObjectMove == 57);
static_assert(kNotifyClash == 58);
static_assert(kNotifyVoiceData == 59);
static_assert(kNotifyPlayerWhereabouts == 60);
static_assert(kNotifyArrowNocked == 61);
static_assert(kNotifyDownedState == 62);
static_assert(kNotifyActionEvents == 63);
static_assert(kNotifyHourMove == 64);
static_assert(kNotifyReferenceEnable == 65);
static_assert(kNotifyWarState == 66);
static_assert(kNotifyPartyPlugin == 67);
static_assert(kNotifyPartyValue == 68);
static_assert(kNotifyNpcPluginsDiffer == 69);
static_assert(kNotifyPlayerName == 70);
static_assert(kNotifyTimeSkip == 71);
// Ours, after upstream's last.
static_assert(kNotifyQuestAliases == 72);
static_assert(kNotifyHitReaction == 73);
static_assert(kNotifyQuestCatchUp == 74);
static_assert(kNotifyPartyFastTravel == 75);
static_assert(kNotifyTrackedQuests == 76);
static_assert(kNotifyDetectionLevels == 77);
static_assert(kNotifyFollowerCommand == 78);
static_assert(kNotifyCrimeReport == 79);
static_assert(kNotifyStuckArrow == 80);
static_assert(kNotifyDismember == 81);
static_assert(kNotifyWordLearned == 82);
static_assert(kNotifyTrapFire == 83);
static_assert(kNotifyDragonSoul == 84);
static_assert(kNotifyCombatTarget == 85);
static_assert(kNotifyDismount == 86);
static_assert(kNotifyModListsDiffer == 87);
static_assert(kNotifyNpcFurniture == 88);
static_assert(kNotifyNpcHeadTrack == 89);
static_assert(kNotifyRagdollPose == 90);
static_assert(kNotifyPropBodies == 91);
static_assert(kNotifyItemTakeLost == 92);
static_assert(kNotifyMapDiscovery == 93);
static_assert(kNotifyActivationCatchUp == 94);
static_assert(kNotifyItemDataChange == 95);
static_assert(kNotifyMagicState == 96);
static_assert(kNotifyPlayerFurniture == 97);
static_assert(kNotifyServerIdentity == 98);
static_assert(kNotifyExtension == 99);
static_assert(kNotifyReferenceStates == 100);
static_assert(kNotifyArrowPickup == 101);
static_assert(kServerOpcodeMax == 102);

namespace
{
/** How many message types each opcode of a factory's enum is read as. */
template <class TFactory, std::size_t N> std::array<uint32_t, N> TypesPerOpcode()
{
    std::array<uint32_t, N> types{};
    TFactory::Visit(
        [&types](auto& aMeta)
        {
            using T = typename std::remove_reference_t<decltype(aMeta)>::Type;
            ++types[T::Opcode];
            return false;
        });
    return types;
}

/** Send a message and read it back through the factory, as the other side of the connection does. */
template <class T, class TFactory> T ThroughFactory(const T& acMessage)
{
    Buffer buffer(1000);
    Buffer::Writer writer(&buffer);
    acMessage.Serialize(writer);
    Buffer::Reader reader(&buffer);
    auto pMessage = TFactory{}.Extract(reader);
    REQUIRE(pMessage);
    REQUIRE(pMessage->GetOpcode() == T::Opcode);
    return *CastUnique<T>(std::move(pMessage));
}
} // namespace

TEST_CASE("every opcode is read as exactly one message type", "[opcode_layout]")
{
    const auto cClient = TypesPerOpcode<ClientMessageFactory, static_cast<std::size_t>(kClientOpcodeMax)>();
    for (std::size_t i = 0; i < cClient.size(); ++i)
    {
        INFO("client opcode " << i);
        CHECK(cClient[i] == 1u);
    }

    const auto cServer = TypesPerOpcode<ServerMessageFactory, static_cast<std::size_t>(kServerOpcodeMax)>();
    for (std::size_t i = 0; i < cServer.size(); ++i)
    {
        INFO("server opcode " << i);
        CHECK(cServer[i] == 1u);
    }
}

// Each added field set at once: a reader that took them in another order, or skipped the speed when not thrown, reads
// the rest wrong.
TEST_CASE("a dropped item's move reads back with its mover, throw and hold all set", "[opcode_layout][droppeditems]")
{
    const glm::vec3 cVelocity(350.f, -120.5f, 410.25f);
    const glm::vec3 cSpin(0.5f, -3.f, 12.f);

    for (const bool cThrown : {false, true})
    {
        INFO("thrown " << cThrown);

        RequestDroppedItemMove request{};
        request.Id = 21;
        request.Position = glm::vec3(100.f, -200.f, 30.f);
        request.Rotation = glm::vec3(0.25f, 0.5f, 1.5f);
        request.AtRest = true;
        request.Tick = 987654321ull;
        request.Thrown = cThrown;
        if (cThrown)
        {
            request.Velocity = cVelocity;
            request.Spin = cSpin;
        }
        request.Fixed = true;
        const auto cRequest = ThroughFactory<RequestDroppedItemMove, ClientMessageFactory>(request);
        CHECK(cRequest == request);
        CHECK(cRequest.Fixed);

        NotifyDroppedItemMove notify{};
        notify.Id = request.Id;
        notify.Position = request.Position;
        notify.Rotation = request.Rotation;
        notify.AtRest = true;
        notify.Tick = request.Tick;
        notify.MoverPlayerId = 0xABCDEF01u;
        notify.Thrown = cThrown;
        notify.Velocity = request.Velocity;
        notify.Spin = request.Spin;
        notify.Fixed = true;
        const auto cNotify = ThroughFactory<NotifyDroppedItemMove, ServerMessageFactory>(notify);
        CHECK(cNotify == notify);
        CHECK(cNotify.MoverPlayerId == 0xABCDEF01u);
        CHECK(cNotify.Fixed);
    }
}

TEST_CASE("a dropped item sent to a player who comes later reads back held still", "[opcode_layout][droppeditems]")
{
    NotifyDroppedItem item{};
    item.Id = 4;
    item.Item.BaseId = GameId(0, 0x12EB7); // Iron Sword
    item.Item.Count = 1;
    item.CellId = GameId(0, 0x9);
    item.WorldSpaceId = GameId(0, 0x3C);
    item.Position = glm::vec3(1.f, 2.f, 3.f);
    item.Rotation = glm::vec3(0.f, 0.f, 1.25f);
    item.RequestId = 7;
    item.LegacyItemData = true;
    item.Replay = true;
    item.Fixed = true;
    const auto cItem = ThroughFactory<NotifyDroppedItem, ServerMessageFactory>(item);
    CHECK(cItem == item);
    CHECK(cItem.Replay);
    CHECK(cItem.Fixed);
}

TEST_CASE("a placed object's move reads back with its mover and throw set", "[opcode_layout][droppeditems]")
{
    const glm::vec3 cVelocity(350.f, -120.5f, 410.25f);
    const glm::vec3 cSpin(0.5f, -3.f, 12.f);

    RequestWorldObjectMove request{};
    request.ObjectId = GameId(1, 0x1234);
    request.CellId = GameId(0, 0x9);
    request.WorldSpaceId = GameId(0, 0x3C);
    request.Position = glm::vec3(-50.f, 70.f, 10.f);
    request.Rotation = glm::vec3(1.f, 2.f, 3.f);
    request.Taken = true;
    request.Tick = 77;
    request.Thrown = true;
    request.Velocity = cVelocity;
    request.Spin = cSpin;
    CHECK(ThroughFactory<RequestWorldObjectMove, ClientMessageFactory>(request) == request);

    NotifyWorldObjectMove notify{};
    notify.ObjectId = request.ObjectId;
    notify.CellId = request.CellId;
    notify.WorldSpaceId = request.WorldSpaceId;
    notify.Position = request.Position;
    notify.Rotation = request.Rotation;
    notify.Taken = true;
    notify.Tick = 77;
    notify.MoverPlayerId = 5;
    notify.Thrown = true;
    notify.Velocity = cVelocity;
    notify.Spin = cSpin;
    CHECK(ThroughFactory<NotifyWorldObjectMove, ServerMessageFactory>(notify) == notify);
}

TEST_CASE("An arrow pick-up goes through both factories unchanged", "[opcode_layout][arrow_pickup]")
{
    RequestArrowPickup request{};
    request.ProjectileBaseId = GameId(0, 0x3BE11);
    request.CellId = GameId(0, 0x1A26F);
    request.X = -1234.5f;
    request.Y = 987.25f;
    request.Z = -12.f;
    CHECK(ThroughFactory<RequestArrowPickup, ClientMessageFactory>(request) == request);

    NotifyArrowPickup notify{};
    notify.ProjectileBaseId = GameId(3, 0x812);
    notify.CellId = request.CellId;
    notify.X = request.X;
    notify.Y = request.Y;
    notify.Z = request.Z;
    CHECK(ThroughFactory<NotifyArrowPickup, ServerMessageFactory>(notify) == notify);
}
