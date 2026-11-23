#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <optional>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "StringCache.h"
#include "Messages/StringCacheUpdate.h"

#include <catch2/catch.hpp>

#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <DecodeGuard.h>
#include <BoundedString.h>
#include <Structs/Factions.h>
#include <Structs/Mods.h>
#include <Messages/AuthenticationRequest.h>
#include <Messages/RequestActionEvents.h>
#include <Messages/NotifyActionEvents.h>
#include <Messages/RequestFactionsChanges.h>
#include <Structs/ActorValueLimits.h>
#include <Structs/ActorValues.h>
#include <Structs/AnimationVariables.h>
#include <Messages/RequestActorValueChanges.h>
#include <Messages/RequestActorMaxValueChanges.h>
#include <Messages/NotifyActorValueChanges.h>
#include <Messages/NotifyActorMaxValueChanges.h>
#include <Structs/Vector2_NetQuantize.h>

#include <TiltedCore/Math.hpp>
#include <TiltedCore/Platform.hpp>

using namespace TiltedPhoques;

TEST_CASE("Encoding factory", "[encoding.factory]")
{
    Buffer buff(1000);

    {
        AuthenticationRequest request;
        request.Token = "TesSt";

        Buffer::Writer writer(&buff);
        request.Serialize(writer);

        Buffer::Reader reader(&buff);

        const ClientMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == request.GetOpcode());

        auto pRequest = CastUnique<AuthenticationRequest>(std::move(pMessage));
        REQUIRE(pRequest->Token == request.Token);
    }

    {
        PartyAcceptInviteRequest request;
        request.InviterId = 123456;

        Buffer::Writer writer(&buff);
        request.Serialize(writer);

        Buffer::Reader reader(&buff);

        const ClientMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == request.GetOpcode());

        auto pRequest = CastUnique<PartyAcceptInviteRequest>(std::move(pMessage));
        REQUIRE(pRequest->InviterId == request.InviterId);
    }
}

TEST_CASE("Static structures", "[encoding.static]")
{
    GIVEN("GameId")
    {
        GameId sendObjects, recvObjects;
        sendObjects.ModId = 1456987;
        sendObjects.BaseId = 0x789654;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }

    GIVEN("Vector3_NetQuantize")
    {
        Vector3_NetQuantize sendObjects, recvObjects;
        sendObjects.x = 142.56f;
        sendObjects.y = 45687.7f;
        sendObjects.z = -142.56f;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }

    GIVEN("Vector2_NetQuantize")
    {
        Vector2_NetQuantize sendObjects, recvObjects;
        sendObjects.x = 1000.89f;
        sendObjects.y = -485632.75f;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }

    GIVEN("Rotator2_NetQuantize")
    {
        Rotator2_NetQuantize sendObjects, recvObjects;
        sendObjects.x = 1.89f;
        sendObjects.y = TiltedPhoques::Pi * 2.0f;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }

    GIVEN("Rotator2_NetQuantize needing wrap")
    {
        // This test is a bit dangerous as floating errors can lead to sendObjects != recvObjects but the difference is minuscule so we don't care abut such cases
        Rotator2_NetQuantize sendObjects, recvObjects;
        sendObjects.x = -1.87f;
        sendObjects.y = static_cast<float>(TiltedPhoques::Pi) * 18.0f + 3.6f;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }
}

TEST_CASE("Differential structures", "[encoding.differential]")
{
    GIVEN("Full ActionEvent")
    {
        ActionEvent sendAction, recvAction;

        sendAction.ActionId = 42;
        sendAction.State1 = 6547;
        sendAction.Tick = 48;
        sendAction.ActorId = 12345678;
        sendAction.EventName = "test";
        sendAction.IdleId = 87964;
        sendAction.State2 = 8963;
        sendAction.TargetEventName = "toast";
        sendAction.TargetId = 963741;
        sendAction.Type = 4;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendAction.GenerateDifferential(recvAction, writer);

            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);

            REQUIRE(sendAction == recvAction);
        }

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendAction.EventName = "Plot twist !";

            sendAction.GenerateDifferential(recvAction, writer);

            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);

            REQUIRE(sendAction == recvAction);
        }
    }

    GIVEN("An ActionEvent with its forms as server mod ids")
    {
        ActionEvent sendAction, recvAction;
        sendAction.Tick = 48;
        sendAction.ActionId = 0x13005;
        sendAction.TargetId = 0x05001234;
        sendAction.IdleId = 0xFE002ABC;
        sendAction.EventName = "attackStart";
        sendAction.ModActionId = GameId(0, 0x13005);
        sendAction.ModTargetId = GameId(7, 0x1234);
        sendAction.ModIdleId = GameId(0x1C3, 0xABC);

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);
            sendAction.GenerateDifferential(recvAction, writer);
            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);
            REQUIRE(sendAction == recvAction);
            REQUIRE(recvAction.ModTargetId == GameId(7, 0x1234));
            REQUIRE(recvAction.ModIdleId == GameId(0x1C3, 0xABC));
        }

        // Only the changed one goes; the others are kept from the previous action.
        {
            ActionEvent next = sendAction;
            next.Tick = 50;
            next.ModTargetId = GameId(0xFFFFFFFF, 0xD0D);
            Buffer buff(1000);
            Buffer::Writer writer(&buff);
            next.GenerateDifferential(sendAction, writer);
            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);
            REQUIRE(next == recvAction);
            REQUIRE(recvAction.ModActionId == GameId(0, 0x13005));
            REQUIRE(recvAction.ModTargetId == GameId(0xFFFFFFFF, 0xD0D));
        }

        // Cleared again, they read as none.
        {
            ActionEvent cleared = recvAction;
            cleared.Tick = 52;
            cleared.ModActionId = {};
            cleared.ModTargetId = {};
            cleared.ModIdleId = {};
            Buffer buff(1000);
            Buffer::Writer writer(&buff);
            cleared.GenerateDifferential(recvAction, writer);
            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);
            REQUIRE(cleared == recvAction);
            REQUIRE_FALSE(recvAction.ModActionId);
            REQUIRE_FALSE(recvAction.ModTargetId);
            REQUIRE_FALSE(recvAction.ModIdleId);
        }

        // A different mod id is a different action.
        ActionEvent other = sendAction;
        other.ModTargetId = GameId(8, 0x1234);
        REQUIRE(other != sendAction);
    }

    GIVEN("A single cached event name")
    {
        ActionEvent sendAction, recvAction;

        TP_UNUSED(StringCache::Get().Add("test"))

        sendAction.ActionId = 42;
        sendAction.State1 = 6547;
        sendAction.Tick = 48;
        sendAction.ActorId = 12345678;
        sendAction.EventName = "test";
        sendAction.IdleId = 87964;
        sendAction.State2 = 8963;
        sendAction.TargetEventName = "toast";
        sendAction.TargetId = 963741;
        sendAction.Type = 4;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendAction.GenerateDifferential(recvAction, writer);

            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);

            REQUIRE(sendAction == recvAction);
        }

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendAction.EventName = "Plot twist !";

            sendAction.GenerateDifferential(recvAction, writer);

            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);

            REQUIRE(sendAction == recvAction);
        }
    }

    GIVEN("Full Mods")
    {
        Mods sendMods, recvMods;

        Buffer buff(1000);
        Buffer::Writer writer(&buff);

        sendMods.ModList.push_back({"Hello", 42});
        sendMods.ModList.push_back({"Hi", 14});
        sendMods.ModList.push_back({"Test", 8});
        sendMods.ModList.push_back({"Toast", 49});

        sendMods.Serialize(writer);

        Buffer::Reader reader(&buff);
        recvMods.Deserialize(reader);

        REQUIRE(sendMods == recvMods);
    }

    GIVEN("AnimationVariables")
    {
        AnimationVariables vars, recvVars;
 
        vars.Booleans.resize(76);
        String testString("\xDE\xAD\xBE\xEF"
                          "\xDE\xAD\xBE\xEF\x76\xB");
        vars.String_to_VectorBool(testString, vars.Booleans);

        vars.Floats.push_back(1.f);
        vars.Floats.push_back(7.f);
        vars.Floats.push_back(12.f);
        vars.Floats.push_back(0.f);
        vars.Floats.push_back(145.f);
        vars.Floats.push_back(100.f);
        vars.Floats.push_back(-1.f);

        vars.Integers.push_back(0);
        vars.Integers.push_back(12000);
        vars.Integers.push_back(06);
        vars.Integers.push_back(7778);
        vars.Integers.push_back(41104539);

        Buffer buff(1000);
        {
            Buffer::Writer writer(&buff);

            vars.GenerateDiff(recvVars, writer);

            Buffer::Reader reader(&buff);
            recvVars.ApplyDiff(reader);

            REQUIRE(vars.Booleans == recvVars.Booleans);
            REQUIRE(vars.Floats == recvVars.Floats);
            REQUIRE(vars.Integers == recvVars.Integers);
        }

        vars.Booleans.resize(33);
        vars.Booleans[16] = false;
        vars.Booleans[17] = false;
        vars.Booleans[18] = false;
        vars.Booleans[19] = false;
        vars.Floats[3] = 42.f;
        vars.Integers[0] = 18;
        vars.Integers[3] = 0;

        {
            Buffer::Writer writer(&buff);

            vars.GenerateDiff(recvVars, writer);

            Buffer::Reader reader(&buff);
            recvVars.ApplyDiff(reader);

            REQUIRE(vars.Booleans == recvVars.Booleans);
            REQUIRE(vars.Floats == recvVars.Floats);
            REQUIRE(vars.Integers == recvVars.Integers);
        }
    }
}

TEST_CASE("Packets", "[encoding.packets]")
{
    SECTION("AuthenticationRequest")
    {
        Buffer buff(1000);

        AuthenticationRequest sendMessage, recvMessage;
        sendMessage.Token = "TesSt";
        sendMessage.Version = "v1.8.1-104-g9da1af5c-dirty.f8f11b7";
        sendMessage.Protocol = "0a1b2c3d";
        sendMessage.HideFromPublicPage = true;
        sendMessage.ContentSizes.GameEdition = ModContentSizes::Edition::kSE;
        sendMessage.ContentSizes.Sizes = {100, 200, 300, 400};
        sendMessage.UserMods.ModList.push_back({"Hello", 42});
        sendMessage.UserMods.ModList.push_back({"Hi", 14});
        sendMessage.UserMods.ModList.push_back({"Test", 8});
        sendMessage.UserMods.ModList.push_back({"Toast", 49});

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }

    SECTION("AuthenticationResponse")
    {
        Buffer buff(1000);

        AuthenticationResponse sendMessage, recvMessage;
        sendMessage.Type = AuthenticationResponse::ResponseType::kAccepted;
        sendMessage.UserMods.ModList.push_back({"Hello", 42});
        sendMessage.UserMods.ModList.push_back({"Hi", 14});
        sendMessage.UserMods.ModList.push_back({"Test", 8});
        sendMessage.UserMods.ModList.push_back({"Toast", 49});

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }

    SECTION("AssignCharacterRequest")
    {
        Buffer buff(1000);

        ActionEvent sendAction;
        sendAction.ActionId = 42;
        sendAction.State1 = 6547;
        sendAction.Tick = 48;
        sendAction.ActorId = 12345678;
        sendAction.EventName = "test";
        sendAction.IdleId = 87964;
        sendAction.State2 = 8963;
        sendAction.TargetEventName = "toast";
        sendAction.TargetId = 963741;
        sendAction.Type = 4;

        AssignCharacterRequest sendMessage, recvMessage;
        sendMessage.Cookie = 14523698;
        sendMessage.AppearanceBuffer = "toto";
        sendMessage.CellId.BaseId = 45;
        sendMessage.FormId.ModId = 48;
        sendMessage.ReferenceId.BaseId = 456799;
        sendMessage.ReferenceId.ModId = 4079;
        sendMessage.LatestAction = sendAction;
        sendMessage.Position.x = -452.4f;
        sendMessage.Position.y = 452.4f;
        sendMessage.Position.z = 125452.4f;
        sendMessage.Rotation.x = -1.87f;
        sendMessage.Rotation.y = 45.35f;
        sendMessage.IsFollower = true;

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }

    GIVEN("ClientReferencesMoveRequest")
    {
        ClientReferencesMoveRequest sendMessage, recvMessage;
        auto& update = sendMessage.Updates[1];
        auto& move = update.UpdatedMovement;

        AnimationVariables vars;
        vars.Booleans.resize(76);
        String testString("\xDE\xAD\xBE\xEF\x76\xB");
        vars.String_to_VectorBool(testString, vars.Booleans);

        vars.Floats.push_back(1.f);
        vars.Floats.push_back(7.f);
        vars.Floats.push_back(12.f);
        vars.Floats.push_back(0.f);
        vars.Floats.push_back(145.f);
        vars.Floats.push_back(100.f);
        vars.Floats.push_back(-1.f);

        vars.Integers.push_back(0);
        vars.Integers.push_back(12000);
        vars.Integers.push_back(06);
        vars.Integers.push_back(7778);
        vars.Integers.push_back(41104539);

        move.Variables = vars;

        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(recvMessage.Updates[1].UpdatedMovement == sendMessage.Updates[1].UpdatedMovement);
    }

    SECTION("AssignObjectsResponse")
    {
        Buffer buff(1000);

        AssignObjectsResponse sendMessage, recvMessage;
        ObjectData door;
        door.ServerId = 1234;
        door.Id = GameId(0, 0x9B0A5);
        door.CurrentLockData.IsLocked = true;
        door.CurrentLockData.LockLevel = 25;
        door.DoorOpenState = 1;
        sendMessage.Objects.push_back(door);
        ObjectData chest;
        chest.ServerId = 1235;
        chest.Id = GameId(0, 0x3EF03);
        chest.IsSenderFirst = true;
        chest.IsVendor = true;
        sendMessage.Objects.push_back(chest);

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
        REQUIRE_FALSE(recvMessage.Objects[0].IsVendor);
        REQUIRE(recvMessage.Objects[1].IsVendor);
        REQUIRE(recvMessage.Objects[1].IsSenderFirst);
        REQUIRE(recvMessage.Objects[0].DoorOpenState == 1);
        REQUIRE(recvMessage.Objects[1].DoorOpenState == 0);
    }

    SECTION("AssignObjectsRequest")
    {
        Buffer buff(1000);

        AssignObjectsRequest sendMessage, recvMessage;
        // A chest of the next cell over, in its own cell, and a merchant's chest.
        ObjectData chest;
        chest.Id = GameId(0, 0x34EE1);
        chest.CellId = GameId(0, 0x9DB5);
        chest.WorldSpaceId = GameId(0, 0x3C);
        chest.CurrentCoords = GridCellCoords(5, -12);
        Inventory::Entry gold{};
        gold.BaseId = GameId(0, 0xF);
        gold.Count = 5;
        chest.CurrentInventory.Entries.push_back(gold);
        sendMessage.Objects.push_back(chest);
        ObjectData merchantChest = chest;
        merchantChest.Id = GameId(0, 0x1A6C0);
        merchantChest.IsVendor = true;
        sendMessage.Objects.push_back(merchantChest);

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
        REQUIRE_FALSE(recvMessage.Objects[0].IsVendor);
        REQUIRE(recvMessage.Objects[1].IsVendor);
        REQUIRE(recvMessage.Objects[0].CellId == GameId(0, 0x9DB5));
        REQUIRE(recvMessage.Objects[0].CurrentInventory.Entries.size() == 1);
    }

    SECTION("RequestObjectInventoryChanges")
    {
        Buffer buff(1000);

        RequestObjectInventoryChanges sendMessage, recvMessage;
        ObjectData merchantChest;
        merchantChest.Id = GameId(0, 0x1A6C0);
        merchantChest.IsVendor = true;
        sendMessage.Changes[merchantChest.Id] = merchantChest;
        ObjectData chest;
        chest.Id = GameId(0, 0x34EE1);
        sendMessage.Changes[chest.Id] = chest;

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
        REQUIRE(recvMessage.Changes[GameId(0, 0x1A6C0)].IsVendor);
        REQUIRE_FALSE(recvMessage.Changes[GameId(0, 0x34EE1)].IsVendor);
    }

    SECTION("RequestActionEvents and NotifyActionEvents")
    {
        ActionEvent swing;
        swing.ActionId = 42;
        swing.State1 = 6547;
        swing.Tick = 48;
        swing.ActorId = 12345678;
        swing.EventName = "attackStart";
        swing.TargetEventName = "toast";
        swing.TargetId = 963741;
        swing.Type = 4;
        swing.ModActionId = GameId(0, 42);
        swing.ModTargetId = GameId(7, 0xB2C9D);
        ActionEvent block = swing;
        block.Tick = 52;
        block.EventName = "blockStart";

        {
            Buffer buff(1000);
            RequestActionEvents sendMessage, recvMessage;
            sendMessage.Actions[7] = {swing, block};
            sendMessage.Actions[100003] = {block};

            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            const ClientMessageFactory factory;
            auto pMessage = factory.Extract(reader);
            REQUIRE(pMessage);
            REQUIRE(pMessage->GetOpcode() == sendMessage.GetOpcode());
            REQUIRE(*CastUnique<RequestActionEvents>(std::move(pMessage)) == sendMessage);
        }

        {
            Buffer buff(1000);
            NotifyActionEvents sendMessage;
            sendMessage.Actions[7] = {swing, block};

            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            const ServerMessageFactory factory;
            auto pMessage = factory.Extract(reader);
            REQUIRE(pMessage);
            REQUIRE(pMessage->GetOpcode() == sendMessage.GetOpcode());
            const auto pRecv = CastUnique<NotifyActionEvents>(std::move(pMessage));
            REQUIRE(*pRecv == sendMessage);
            // The order the actions were performed in is kept.
            REQUIRE(pRecv->Actions[7][0].EventName == "attackStart");
            REQUIRE(pRecv->Actions[7][1].EventName == "blockStart");
            REQUIRE(pRecv->Actions[7][1].ModTargetId == GameId(7, 0xB2C9D));
        }

        // In a spawn's replay chain too.
        {
            CharacterSpawnRequest spawn;
            spawn.ServerId = 0x1A;
            spawn.OwnershipEpoch = 1;
            spawn.ActionsToReplay.Actions = {swing, block};
            Buffer buff(1000);
            Buffer::Writer writer(&buff);
            spawn.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ServerMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            const auto pRecv = CastUnique<CharacterSpawnRequest>(std::move(pMessage));
            REQUIRE(*pRecv == spawn);
            REQUIRE(pRecv->ActionsToReplay.Actions[0].ModActionId == GameId(0, 42));
        }
    }

    SECTION("RequestPartyPlugin and NotifyPartyPlugin")
    {
        {
            RequestPartyPlugin sendMessage;
            sendMessage.PluginFile = "Some Plugin.esp";

            uint8_t storage[1 << 12];
            Buffer buffer(storage, sizeof(storage));
            Buffer::Writer writer(&buffer);
            sendMessage.Serialize(writer);

            Buffer::Reader reader(&buffer);
            const ClientMessageFactory factory;
            auto pMessage = factory.Extract(reader);

            REQUIRE(pMessage);
            REQUIRE(*CastUnique<RequestPartyPlugin>(std::move(pMessage)) == sendMessage);
        }
        {
            NotifyPartyPlugin sendMessage;
            sendMessage.PluginFile = "Some Plugin.esp";
            sendMessage.PlayerWithout = "player 2";

            uint8_t storage[1 << 12];
            Buffer buffer(storage, sizeof(storage));
            Buffer::Writer writer(&buffer);
            sendMessage.Serialize(writer);

            Buffer::Reader reader(&buffer);
            const ServerMessageFactory factory;
            auto pMessage = factory.Extract(reader);

            REQUIRE(pMessage);
            REQUIRE(*CastUnique<NotifyPartyPlugin>(std::move(pMessage)) == sendMessage);
        }
    }

    SECTION("RequestNpcPlugins and NotifyNpcPluginsDiffer")
    {
        {
            RequestNpcPlugins sendMessage;
            sendMessage.Plugins = {"Skyrim.esm", "Some Creatures.esp"};

            uint8_t storage[1 << 12];
            Buffer buffer(storage, sizeof(storage));
            Buffer::Writer writer(&buffer);
            sendMessage.Serialize(writer);

            Buffer::Reader reader(&buffer);
            const ClientMessageFactory factory;
            auto pMessage = factory.Extract(reader);

            REQUIRE(pMessage);
            REQUIRE(*CastUnique<RequestNpcPlugins>(std::move(pMessage)) == sendMessage);
        }
        {
            NotifyNpcPluginsDiffer sendMessage;
            sendMessage.Username = "player 2";
            sendMessage.OnlyHere = {"Some Creatures.esp"};
            sendMessage.OnlyThere = {"Other Bandits.esp", "More Wolves.esl"};

            uint8_t storage[1 << 12];
            Buffer buffer(storage, sizeof(storage));
            Buffer::Writer writer(&buffer);
            sendMessage.Serialize(writer);

            Buffer::Reader reader(&buffer);
            const ServerMessageFactory factory;
            auto pMessage = factory.Extract(reader);

            REQUIRE(pMessage);
            REQUIRE(*CastUnique<NotifyNpcPluginsDiffer>(std::move(pMessage)) == sendMessage);
        }
    }

    SECTION("NotifyPlayerList with its flat players")
    {
        NotifyPlayerList sendMessage;
        sendMessage.Players[1] = "player 1";
        sendMessage.Players[2] = "player 2";
        sendMessage.Players[7] = "player 3";
        sendMessage.FlatPlayers = {2, 7};

        uint8_t storage[1 << 12];
        Buffer buffer(storage, sizeof(storage));
        Buffer::Writer writer(&buffer);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buffer);
        const ServerMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        REQUIRE(*CastUnique<NotifyPlayerList>(std::move(pMessage)) == sendMessage);
    }

    SECTION("RequestRespawn with a look and a name, and NotifyPlayerName")
    {
        const auto roundTrip = [](const RequestRespawn& acSent)
        {
            uint8_t storage[1 << 12];
            Buffer buffer(storage, sizeof(storage));
            Buffer::Writer writer(&buffer);
            acSent.Serialize(writer);

            Buffer::Reader reader(&buffer);
            const ClientMessageFactory factory;
            auto pMessage = factory.Extract(reader);

            REQUIRE(pMessage);
            REQUIRE(*CastUnique<RequestRespawn>(std::move(pMessage)) == acSent);
        };
        {
            RequestRespawn sendMessage;
            sendMessage.ActorId = 0x200046;
            sendMessage.AppearanceBuffer = String("\x01\x02\x00\x03", 4);
            sendMessage.ChangeFlags = 0x3000A68;
            Tints::Entry tint{};
            tint.Name = R"(Actors\Character\Character Assets\TintMasks\SkinTone.dds)";
            tint.Alpha = 0.5f;
            tint.Color = 0xFF8040;
            tint.Type = 6;
            sendMessage.FaceTints.Entries = {tint, tint};
            sendMessage.Username = "Witch Queen";
            roundTrip(sendMessage);
        }
        {
            // The bare request a game sends back after NotifyRespawn.
            RequestRespawn sendMessage;
            sendMessage.ActorId = 0x200046;
            roundTrip(sendMessage);
        }
        {
            NotifyPlayerName sendMessage;
            sendMessage.PlayerId = 3;
            sendMessage.Username = "Witch Queen";

            uint8_t storage[1 << 12];
            Buffer buffer(storage, sizeof(storage));
            Buffer::Writer writer(&buffer);
            sendMessage.Serialize(writer);

            Buffer::Reader reader(&buffer);
            const ServerMessageFactory factory;
            auto pMessage = factory.Extract(reader);

            REQUIRE(pMessage);
            REQUIRE(*CastUnique<NotifyPlayerName>(std::move(pMessage)) == sendMessage);
        }
    }

    SECTION("RequestPartyValue and NotifyPartyValue")
    {
        {
            RequestPartyValue sendMessage;
            sendMessage.Key = "courtyard";
            sendMessage.Value = 0.625f;

            uint8_t storage[1 << 12];
            Buffer buffer(storage, sizeof(storage));
            Buffer::Writer writer(&buffer);
            sendMessage.Serialize(writer);

            Buffer::Reader reader(&buffer);
            const ClientMessageFactory factory;
            auto pMessage = factory.Extract(reader);

            REQUIRE(pMessage);
            REQUIRE(*CastUnique<RequestPartyValue>(std::move(pMessage)) == sendMessage);
        }
        {
            NotifyPartyValue sendMessage;
            sendMessage.Key = "courtyard";
            sendMessage.Value = -3.5f;

            uint8_t storage[1 << 12];
            Buffer buffer(storage, sizeof(storage));
            Buffer::Writer writer(&buffer);
            sendMessage.Serialize(writer);

            Buffer::Reader reader(&buffer);
            const ServerMessageFactory factory;
            auto pMessage = factory.Extract(reader);

            REQUIRE(pMessage);
            REQUIRE(*CastUnique<NotifyPartyValue>(std::move(pMessage)) == sendMessage);
        }
    }

    SECTION("RequestActorPresence")
    {
        RequestActorPresence sendMessage;
        sendMessage.Actors = {{0x900121, ActorPresenceKind::Own, false, 0x100004}, {0x1038B1, ActorPresenceKind::Copy, true}, {0x300090, ActorPresenceKind::StandIn, false}};

        uint8_t storage[1 << 12];
        Buffer buffer(storage, sizeof(storage));
        Buffer::Writer writer(&buffer);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buffer);
        const ClientMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        const auto pRecv = CastUnique<RequestActorPresence>(std::move(pMessage));
        REQUIRE(*pRecv == sendMessage);
        REQUIRE(pRecv->Actors[2].Kind == ActorPresenceKind::StandIn);
        REQUIRE(pRecv->Actors[1].Dead);
        REQUIRE(pRecv->Actors[0].CombatTarget == 0x100004);
    }

    SECTION("NotifyOwnershipTransfer")
    {
        NotifyOwnershipTransfer sendMessage;
        sendMessage.ServerId = 0x200038;
        sendMessage.OwnerPlayerId = 3;
        sendMessage.OwnershipEpoch = 7;
        sendMessage.CurrentActorData.IsWeaponDrawn = true;
        sendMessage.CombatTarget = 0x100004;
        sendMessage.CellId = GameId(0, 0x9732);
        sendMessage.Position = glm::vec3(12345.f, -6789.f, 512.f);
        sendMessage.Heading = 1.5f;

        uint8_t storage[1 << 12];
        Buffer buffer(storage, sizeof(storage));
        Buffer::Writer writer(&buffer);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buffer);
        const ServerMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        const auto pRecv = CastUnique<NotifyOwnershipTransfer>(std::move(pMessage));
        REQUIRE(*pRecv == sendMessage);
        REQUIRE(pRecv->CombatTarget == 0x100004);
        REQUIRE(pRecv->CellId == GameId(0, 0x9732));
        REQUIRE(pRecv->Position == sendMessage.Position);
    }

    SECTION("RequestReferenceEnable and NotifyReferenceEnable")
    {
        const ReferenceEnableState camp{GameId(0, 0x8680C), true};
        const ReferenceEnableState barricade{GameId(2, 0x8FA11), false};

        {
            Buffer buff(1000);
            RequestReferenceEnable sendMessage;
            sendMessage.Changes = {camp, barricade};

            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ClientMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            REQUIRE(pMessage->GetOpcode() == sendMessage.GetOpcode());
            REQUIRE(*CastUnique<RequestReferenceEnable>(std::move(pMessage)) == sendMessage);
        }

        {
            Buffer buff(1000);
            NotifyReferenceEnable sendMessage;
            sendMessage.Changes = {barricade, camp};

            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ServerMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            const auto pRecv = CastUnique<NotifyReferenceEnable>(std::move(pMessage));
            REQUIRE(*pRecv == sendMessage);
            // The order the switches happened in is kept.
            REQUIRE(pRecv->Changes[0].Id == barricade.Id);
            REQUIRE(pRecv->Changes[1].Enabled);
        }
    }

    SECTION("RequestPartyFastTravel, NotifyPartyFastTravel and RequestPartyFollow")
    {
        {
            Buffer buff(100);
            RequestPartyFastTravel sendMessage;
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ClientMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            REQUIRE(pMessage->GetOpcode() == kRequestPartyFastTravel);
        }

        {
            Buffer buff(100);
            RequestPartyFollow sendMessage;
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ClientMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            REQUIRE(pMessage->GetOpcode() == kRequestPartyFollow);
        }

        {
            Buffer buff(100);
            NotifyPartyFastTravel sendMessage;
            sendMessage.LeaderId = 70000;
            sendMessage.CellId = GameId(0, 0x1A26F);
            sendMessage.WorldSpaceId = GameId(0, 0x3C);
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ServerMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            REQUIRE(*CastUnique<NotifyPartyFastTravel>(std::move(pMessage)) == sendMessage);
        }
    }

    SECTION("RequestTrackedQuests and NotifyTrackedQuests")
    {
        const TiltedPhoques::Vector<TrackedQuest> cQuests{{GameId(0, 0x3372B), 1}, {GameId(2, 0x1A2B), 6}};

        {
            Buffer buff(1000);
            RequestTrackedQuests sendMessage;
            sendMessage.Quests = cQuests;
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ClientMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            REQUIRE(pMessage->GetOpcode() == kRequestTrackedQuests);
            REQUIRE(*CastUnique<RequestTrackedQuests>(std::move(pMessage)) == sendMessage);
        }

        {
            Buffer buff(1000);
            NotifyTrackedQuests sendMessage;
            sendMessage.Quests = cQuests;
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ServerMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            REQUIRE(*CastUnique<NotifyTrackedQuests>(std::move(pMessage)) == sendMessage);
        }

        {
            // A leader that follows none tells its members so.
            Buffer buff(100);
            NotifyTrackedQuests sendMessage;
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ServerMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            REQUIRE(CastUnique<NotifyTrackedQuests>(std::move(pMessage))->Quests.empty());
        }
    }

    SECTION("RequestCrimeReport and NotifyCrimeReport")
    {
        static_assert(kRequestCrimeReport == kRequestFollowerCommand + 1);
        static_assert(kNotifyCrimeReport == kNotifyFollowerCommand + 1);

        const auto cReport = [](size_t aWitnesses)
        {
            CrimeReport report;
            report.Kind = 0;
            report.Ref = GameId(0, 0x1A2B3);
            report.Object = GameId(0, 0x5ACE4);
            report.Owner = GameId(2, 0x13B99);
            report.Count = 3;
            report.Value = 21;
            for (size_t i = 0; i < aWitnesses; ++i)
                report.Witnesses.push_back(static_cast<uint32_t>(0x10000 + i));
            return report;
        };

        for (const size_t cWitnesses : {size_t(0), size_t(1), size_t(16)})
        {
            Buffer buff(1000);
            RequestCrimeReport sendMessage;
            sendMessage.Report = cReport(cWitnesses);
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ClientMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            REQUIRE(pMessage->GetOpcode() == kRequestCrimeReport);
            REQUIRE(*CastUnique<RequestCrimeReport>(std::move(pMessage)) == sendMessage);
        }

        {
            // A theft from a temporary reference, the largest values, and every id at its widest.
            Buffer buff(1000);
            NotifyCrimeReport sendMessage;
            sendMessage.OffenderPlayerId = 0xFFFFFFFF;
            sendMessage.Report = cReport(1);
            sendMessage.Report.Ref = GameId{};
            sendMessage.Report.VictimId = 0xFFFFFFFF;
            sendMessage.Report.Count = 100000;
            sendMessage.Report.Value = 100000000;
            sendMessage.Report.Witnesses[0] = 0xFFFFFFFF;
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ServerMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            REQUIRE(pMessage->GetOpcode() == kNotifyCrimeReport);
            REQUIRE(*CastUnique<NotifyCrimeReport>(std::move(pMessage)) == sendMessage);
        }

        {
            // Seventeen witnesses: no real alarm lists that many.
            Buffer buff(1000);
            RequestCrimeReport sendMessage;
            sendMessage.Report = cReport(17);
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            REQUIRE(!ClientMessageFactory{}.Extract(reader));
        }

        {
            // A kind the game has no number for.
            Buffer buff(1000);
            RequestCrimeReport sendMessage;
            sendMessage.Report = cReport(1);
            sendMessage.Report.Kind = 7;
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            REQUIRE(!ClientMessageFactory{}.Extract(reader));
        }
    }

    SECTION("RequestDetectionLevels and NotifyDetectionLevels")
    {
        const TiltedPhoques::Vector<DetectionWord> cWords{{0x1A2B, 3, 2}, {7, 0x10000, 0}, {0xFFFFFFFF, 1, 1}};

        {
            Buffer buff(1000);
            RequestDetectionLevels sendMessage;
            sendMessage.Words = cWords;
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ClientMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            REQUIRE(pMessage->GetOpcode() == kRequestDetectionLevels);
            REQUIRE(*CastUnique<RequestDetectionLevels>(std::move(pMessage)) == sendMessage);
        }

        {
            Buffer buff(1000);
            NotifyDetectionLevels sendMessage;
            sendMessage.Words = cWords;
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ServerMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            REQUIRE(pMessage->GetOpcode() == kNotifyDetectionLevels);
            REQUIRE(*CastUnique<NotifyDetectionLevels>(std::move(pMessage)) == sendMessage);
        }
    }

    SECTION("RequestWordLearned and NotifyWordLearned")
    {
        {
            Buffer buff(100);
            RequestWordLearned sendMessage;
            sendMessage.WordId = GameId(0, 0x13E22);
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ClientMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            REQUIRE(pMessage->GetOpcode() == kRequestWordLearned);
            REQUIRE(*CastUnique<RequestWordLearned>(std::move(pMessage)) == sendMessage);
        }

        {
            Buffer buff(100);
            NotifyWordLearned sendMessage;
            sendMessage.Words = {GameId(2, 0x1A2B)};
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ServerMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            REQUIRE(pMessage->GetOpcode() == kNotifyWordLearned);
            REQUIRE(*CastUnique<NotifyWordLearned>(std::move(pMessage)) == sendMessage);
        }
    }

    SECTION("RequestTrapFire and NotifyTrapFire")
    {
        {
            Buffer buff(1000);
            RequestTrapFire sendMessage;
            sendMessage.Id = GameId(0, 0x10A3F2);
            sendMessage.TriggerId = GameId(0, 0x10A3F0);
            sendMessage.CellId = GameId(0, 0x15A7B);
            sendMessage.WorldSpaceId = GameId(0, 0x3C);
            sendMessage.CenterCoords = GridCellCoords::CalculateGridCellCoords(19000.f, -6000.f);
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ClientMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            REQUIRE(pMessage->GetOpcode() == kRequestTrapFire);
            REQUIRE(*CastUnique<RequestTrapFire>(std::move(pMessage)) == sendMessage);
        }

        {
            Buffer buff(1000);
            NotifyTrapFire sendMessage;
            sendMessage.Id = GameId(2, 0x1A2B);
            sendMessage.TriggerId = GameId(2, 0x1A2C);
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ServerMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            REQUIRE(pMessage->GetOpcode() == kNotifyTrapFire);
            REQUIRE(*CastUnique<NotifyTrapFire>(std::move(pMessage)) == sendMessage);
        }
    }

    SECTION("RequestCombatTarget and NotifyCombatTarget")
    {
        // No words, one, and as many as a message holds; a fight that ended (target 0), and ids with their high bits set.
        TiltedPhoques::Vector<CombatTargetWord> full;
        for (uint32_t i = 0; i < CombatTargetWords::kMax; ++i)
            full.push_back({0x80000000u + i, i % 3 ? 0xFFFFFFF0u - i : 0, i + 1});
        TiltedPhoques::Vector<CombatTargetWord> one;
        one.push_back({0x1A2B, 0, 3});
        TiltedPhoques::Vector<CombatTargetWord> two;
        two.push_back({0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFFFD});
        two.push_back({7, 0x10000, 1});
        const TiltedPhoques::Vector<CombatTargetWord> cCases[] = {{}, one, two, full};

        for (const auto& cWords : cCases)
        {
            {
                Buffer buff(16000);
                RequestCombatTarget sendMessage;
                sendMessage.Words = cWords;
                Buffer::Writer writer(&buff);
                sendMessage.Serialize(writer);
                Buffer::Reader reader(&buff);
                auto pMessage = ClientMessageFactory{}.Extract(reader);
                REQUIRE(pMessage);
                REQUIRE(pMessage->GetOpcode() == kRequestCombatTarget);
                REQUIRE(*CastUnique<RequestCombatTarget>(std::move(pMessage)) == sendMessage);
            }

            {
                Buffer buff(16000);
                NotifyCombatTarget sendMessage;
                sendMessage.Words = cWords;
                Buffer::Writer writer(&buff);
                sendMessage.Serialize(writer);
                Buffer::Reader reader(&buff);
                auto pMessage = ServerMessageFactory{}.Extract(reader);
                REQUIRE(pMessage);
                REQUIRE(pMessage->GetOpcode() == kNotifyCombatTarget);
                REQUIRE(*CastUnique<NotifyCombatTarget>(std::move(pMessage)) == sendMessage);
            }
        }
    }

    SECTION("RequestNpcFurniture and NotifyNpcFurniture")
    {
        // No words, one, and as many as a message holds; an NPC that got up (state 0), and ids with their high bits set.
        TiltedPhoques::Vector<NpcFurnitureWord> full;
        for (uint32_t i = 0; i < NpcFurnitureWords::kMax; ++i)
            full.push_back({0x80000000u + i, i % 3 ? 0xFEu : 0, i % 3 ? 0xFFFFF000u - i : 0, i % 5, static_cast<uint8_t>(i % 3 ? (i % 2 ? 3 : 7) : 0), i + 1});
        TiltedPhoques::Vector<NpcFurnitureWord> one;
        one.push_back({0x1A2B, 0, 0, 0, 0, 3});
        TiltedPhoques::Vector<NpcFurnitureWord> two;
        two.push_back({0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFFFD, 7, 0xFFFFFFFC});
        two.push_back({7, 2, 0x1A2B3, 1, 3, 1});
        const TiltedPhoques::Vector<NpcFurnitureWord> cCases[] = {{}, one, two, full};

        for (const auto& cWords : cCases)
        {
            {
                Buffer buff(20000);
                RequestNpcFurniture sendMessage;
                sendMessage.Words = cWords;
                Buffer::Writer writer(&buff);
                sendMessage.Serialize(writer);
                Buffer::Reader reader(&buff);
                auto pMessage = ClientMessageFactory{}.Extract(reader);
                REQUIRE(pMessage);
                REQUIRE(pMessage->GetOpcode() == kRequestNpcFurniture);
                REQUIRE(*CastUnique<RequestNpcFurniture>(std::move(pMessage)) == sendMessage);
            }

            {
                Buffer buff(20000);
                NotifyNpcFurniture sendMessage;
                sendMessage.Words = cWords;
                Buffer::Writer writer(&buff);
                sendMessage.Serialize(writer);
                Buffer::Reader reader(&buff);
                auto pMessage = ServerMessageFactory{}.Extract(reader);
                REQUIRE(pMessage);
                REQUIRE(pMessage->GetOpcode() == kNotifyNpcFurniture);
                REQUIRE(*CastUnique<NotifyNpcFurniture>(std::move(pMessage)) == sendMessage);
            }
        }
    }

    SECTION("An NPC furniture count no message holds")
    {
        TiltedPhoques::Vector<NpcFurnitureWord> tooMany(NpcFurnitureWords::kMax + 1, NpcFurnitureWord{7, 2, 0x1A2B3, 1, 3, 3});

        {
            Buffer buff(20000);
            RequestNpcFurniture sendMessage;
            sendMessage.Words = tooMany;
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            REQUIRE(!ClientMessageFactory{}.Extract(reader));
            REQUIRE(DecodeGuard::t_failed);
            DecodeGuard::t_failed = false;
        }

        {
            Buffer buff(20000);
            NotifyNpcFurniture sendMessage;
            sendMessage.Words = tooMany;
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            REQUIRE(!ServerMessageFactory{}.Extract(reader));
            REQUIRE(DecodeGuard::t_failed);
            DecodeGuard::t_failed = false;
        }
    }

    SECTION("RequestNpcHeadTrack and NotifyNpcHeadTrack")
    {
        // No words, one, and as many as a message holds; a stop (flags 0), negative and far points in quarter units, and ids with their high bits set.
        TiltedPhoques::Vector<NpcHeadTrackWord> full;
        for (uint32_t i = 0; i < NpcHeadTrackWords::kMax; ++i)
            full.push_back({0x80000000u + i, static_cast<uint8_t>(i % 4), -150000.25f + static_cast<float>(i), 0.75f * static_cast<float>(i), -2.5f, i + 1});
        TiltedPhoques::Vector<NpcHeadTrackWord> one;
        one.push_back({0x1A2B, 0, 0.f, 0.f, 0.f, 3});
        TiltedPhoques::Vector<NpcHeadTrackWord> two;
        two.push_back({0xFFFFFFFF, 3, 262143.75f, -262143.75f, 0.25f, 0xFFFFFFFC});
        two.push_back({7, 1, 1200.f, -340.5f, 8100.25f, 1});
        const TiltedPhoques::Vector<NpcHeadTrackWord> cCases[] = {{}, one, two, full};

        for (const auto& cWords : cCases)
        {
            {
                Buffer buff(30000);
                RequestNpcHeadTrack sendMessage;
                sendMessage.Words = cWords;
                Buffer::Writer writer(&buff);
                sendMessage.Serialize(writer);
                Buffer::Reader reader(&buff);
                auto pMessage = ClientMessageFactory{}.Extract(reader);
                REQUIRE(pMessage);
                REQUIRE(pMessage->GetOpcode() == kRequestNpcHeadTrack);
                REQUIRE(*CastUnique<RequestNpcHeadTrack>(std::move(pMessage)) == sendMessage);
            }

            {
                Buffer buff(30000);
                NotifyNpcHeadTrack sendMessage;
                sendMessage.Words = cWords;
                Buffer::Writer writer(&buff);
                sendMessage.Serialize(writer);
                Buffer::Reader reader(&buff);
                auto pMessage = ServerMessageFactory{}.Extract(reader);
                REQUIRE(pMessage);
                REQUIRE(pMessage->GetOpcode() == kNotifyNpcHeadTrack);
                REQUIRE(*CastUnique<NotifyNpcHeadTrack>(std::move(pMessage)) == sendMessage);
            }
        }
    }

    SECTION("A look point is sent to a quarter of a unit, and a point that is not a number as 0")
    {
        CHECK(NpcHeadTrackWords::UnpackCoordinate(NpcHeadTrackWords::PackCoordinate(1234.3f)) == 1234.25f);
        CHECK(NpcHeadTrackWords::UnpackCoordinate(NpcHeadTrackWords::PackCoordinate(-0.2f)) == -0.25f);
        CHECK(NpcHeadTrackWords::UnpackCoordinate(NpcHeadTrackWords::PackCoordinate(std::numeric_limits<float>::quiet_NaN())) == 0.f);
        CHECK(NpcHeadTrackWords::UnpackCoordinate(NpcHeadTrackWords::PackCoordinate(std::numeric_limits<float>::infinity())) == 0.f);
        CHECK(NpcHeadTrackWords::UnpackCoordinate(NpcHeadTrackWords::PackCoordinate(1e12f)) == 0.f);
    }

    SECTION("A look count no message holds")
    {
        TiltedPhoques::Vector<NpcHeadTrackWord> tooMany(NpcHeadTrackWords::kMax + 1, NpcHeadTrackWord{7, 1, 1.f, 2.f, 3.f, 3});

        {
            Buffer buff(30000);
            RequestNpcHeadTrack sendMessage;
            sendMessage.Words = tooMany;
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            REQUIRE(!ClientMessageFactory{}.Extract(reader));
            REQUIRE(DecodeGuard::t_failed);
            DecodeGuard::t_failed = false;
        }

        {
            Buffer buff(30000);
            NotifyNpcHeadTrack sendMessage;
            sendMessage.Words = tooMany;
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            REQUIRE(!ServerMessageFactory{}.Extract(reader));
            REQUIRE(DecodeGuard::t_failed);
            DecodeGuard::t_failed = false;
        }
    }

    SECTION("A combat target count no message holds")
    {
        TiltedPhoques::Vector<CombatTargetWord> tooMany(CombatTargetWords::kMax + 1, CombatTargetWord{7, 20, 3});

        {
            Buffer buff(16000);
            RequestCombatTarget sendMessage;
            sendMessage.Words = tooMany;
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            REQUIRE(!ClientMessageFactory{}.Extract(reader));
            REQUIRE(DecodeGuard::t_failed);
            DecodeGuard::t_failed = false;
        }

        {
            Buffer buff(16000);
            NotifyCombatTarget sendMessage;
            sendMessage.Words = tooMany;
            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            REQUIRE(!ServerMessageFactory{}.Extract(reader));
            REQUIRE(DecodeGuard::t_failed);
            DecodeGuard::t_failed = false;
        }
    }

    SECTION("RequestQuestCatchUp")
    {
        Buffer buff(100);
        RequestQuestCatchUp sendMessage;
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);
        Buffer::Reader reader(&buff);
        auto pMessage = ClientMessageFactory{}.Extract(reader);
        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == kRequestQuestCatchUp);
    }

    SECTION("RequestSaveLoaded")
    {
        Buffer buff(100);
        RequestSaveLoaded sendMessage;
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);
        Buffer::Reader reader(&buff);
        auto pMessage = ClientMessageFactory{}.Extract(reader);
        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == kRequestSaveLoaded);
        REQUIRE(*CastUnique<RequestSaveLoaded>(std::move(pMessage)) == sendMessage);
    }

    SECTION("NotifyQuestCatchUp")
    {
        Buffer buff(1000);
        NotifyQuestCatchUp sendMessage;
        sendMessage.Steps = {
            {GameId(0, 0x3372B), 0, NotifyQuestUpdate::Started, 1},
            {GameId(2, 0x1A2B), 4100, NotifyQuestUpdate::StageUpdate, 6},
            {GameId(0, 0x3372B), 200, NotifyQuestUpdate::Stopped, 1}};

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);
        Buffer::Reader reader(&buff);
        auto pMessage = ServerMessageFactory{}.Extract(reader);
        REQUIRE(pMessage);
        const auto pRecv = CastUnique<NotifyQuestCatchUp>(std::move(pMessage));
        REQUIRE(*pRecv == sendMessage);
        // Applied in the order the party was sent them.
        REQUIRE(pRecv->Steps[1].Stage == 4100);
        REQUIRE(pRecv->Steps[2].Status == NotifyQuestUpdate::Stopped);
    }

    SECTION("RequestWarState and NotifyWarState")
    {
        WarState state{};
        state.Owners[4] = 2;
        state.Objectives[11] = WarState::kDisplayed | WarState::kCompleted;
        state.Objectives.back() = WarState::kFailed;

        {
            Buffer buff(1000);
            RequestWarState sendMessage;
            sendMessage.State = state;

            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ClientMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            REQUIRE(pMessage->GetOpcode() == sendMessage.GetOpcode());
            REQUIRE(*CastUnique<RequestWarState>(std::move(pMessage)) == sendMessage);
        }

        {
            Buffer buff(1000);
            NotifyWarState sendMessage;
            sendMessage.State = state;

            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ServerMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            const auto pRecv = CastUnique<NotifyWarState>(std::move(pMessage));
            REQUIRE(*pRecv == sendMessage);
            REQUIRE(pRecv->State.Owners[4] == 2);
            REQUIRE(pRecv->State.Objectives.back() == WarState::kFailed);
        }
    }

    SECTION("NotifyRemoveCharacter")
    {
        for (const bool cGone : {false, true})
        {
            Buffer buff(100);
            NotifyRemoveCharacter sendMessage;
            sendMessage.ServerId = 0x100005;
            sendMessage.Gone = cGone;

            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);
            Buffer::Reader reader(&buff);
            auto pMessage = ServerMessageFactory{}.Extract(reader);
            REQUIRE(pMessage);
            const auto pRecv = CastUnique<NotifyRemoveCharacter>(std::move(pMessage));
            REQUIRE(*pRecv == sendMessage);
            REQUIRE(pRecv->Gone == cGone);
        }
    }

    SECTION("A character revived since its last death")
    {
        for (const bool cRevived : {false, true})
        {
            AssignCharacterRequest announce;
            announce.Cookie = 7;
            announce.CurrentActorData.RevivedSinceDeath = cRevived;
            Buffer announceBuffer(1000);
            Buffer::Writer announceWriter(&announceBuffer);
            announce.Serialize(announceWriter);
            Buffer::Reader announceReader(&announceBuffer);
            auto pAnnounce = ClientMessageFactory{}.Extract(announceReader);
            REQUIRE(pAnnounce);
            const auto pAnnounced = CastUnique<AssignCharacterRequest>(std::move(pAnnounce));
            REQUIRE(*pAnnounced == announce);
            REQUIRE(pAnnounced->CurrentActorData.RevivedSinceDeath == cRevived);

            CharacterSpawnRequest spawn;
            spawn.ServerId = 0x1A;
            spawn.FollowerOfPlayerId = 3;
            spawn.RevivedSinceDeath = cRevived;
            Buffer spawnBuffer(1000);
            Buffer::Writer spawnWriter(&spawnBuffer);
            spawn.Serialize(spawnWriter);
            Buffer::Reader spawnReader(&spawnBuffer);
            auto pSpawn = ServerMessageFactory{}.Extract(spawnReader);
            REQUIRE(pSpawn);
            const auto pSpawned = CastUnique<CharacterSpawnRequest>(std::move(pSpawn));
            REQUIRE(*pSpawned == spawn);
            REQUIRE(pSpawned->RevivedSinceDeath == cRevived);
            REQUIRE(pSpawned->FollowerOfPlayerId == 3);

            AssignCharacterResponse answer;
            answer.Cookie = 9;
            answer.ServerId = 0x100014;
            answer.RevivedSinceDeath = cRevived;
            answer.LeveledNpcPickId.BaseId = 0x770B1;
            Buffer answerBuffer(1000);
            Buffer::Writer answerWriter(&answerBuffer);
            answer.Serialize(answerWriter);
            Buffer::Reader answerReader(&answerBuffer);
            auto pAnswer = ServerMessageFactory{}.Extract(answerReader);
            REQUIRE(pAnswer);
            const auto pAnswered = CastUnique<AssignCharacterResponse>(std::move(pAnswer));
            REQUIRE(*pAnswered == answer);
            REQUIRE(pAnswered->RevivedSinceDeath == cRevived);
            REQUIRE(pAnswered->LeveledNpcPickId.BaseId == 0x770B1);
        }
    }

    SECTION("A player's character says which of that player's characters it is")
    {
        for (const uint32_t cGeneration : {0u, 1u, 300u, 0xFFFFFFFFu})
        {
            CharacterSpawnRequest spawn;
            spawn.ServerId = 0x1A;
            spawn.OwnershipEpoch = 1;
            spawn.PlayerId = 2;
            spawn.IsPlayer = true;
            spawn.DismemberedLimbs = 0x8001;
            spawn.SpawnGeneration = cGeneration;
            Buffer buffer(1000);
            Buffer::Writer writer(&buffer);
            spawn.Serialize(writer);
            Buffer::Reader reader(&buffer);
            auto pSpawn = ServerMessageFactory{}.Extract(reader);
            REQUIRE(pSpawn);
            const auto pSpawned = CastUnique<CharacterSpawnRequest>(std::move(pSpawn));
            REQUIRE(*pSpawned == spawn);
            REQUIRE(pSpawned->SpawnGeneration == cGeneration);
            // Written after the limbs, which still read as before.
            REQUIRE(pSpawned->DismemberedLimbs == 0x8001);
        }

        CharacterSpawnRequest older, newer;
        older.SpawnGeneration = 4;
        newer.SpawnGeneration = 5;
        REQUIRE_FALSE(older == newer);
    }
}

TEST_CASE("Damaged messages are dropped", "[encoding.damaged]")
{
    SECTION("A reference count no message holds")
    {
        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        writer.WriteBits(kNotifyReferenceEnable, 8);
        Serialization::WriteVarInt(writer, 1ull << 40);

        Buffer::Reader reader(&buff);
        REQUIRE(!ServerMessageFactory{}.Extract(reader));
    }

    SECTION("An action count no message holds")
    {
        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        writer.WriteBits(kRequestActionEvents, 8);
        Serialization::WriteVarInt(writer, 1);
        Serialization::WriteVarInt(writer, 7);
        Serialization::WriteVarInt(writer, 1ull << 40);

        Buffer::Reader reader(&buff);
        REQUIRE(!ClientMessageFactory{}.Extract(reader));
    }

    SECTION("An actor named twice")
    {
        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        writer.WriteBits(kNotifyActionEvents, 8);
        Serialization::WriteVarInt(writer, 2);
        Serialization::WriteVarInt(writer, 7);
        Serialization::WriteVarInt(writer, 0);
        Serialization::WriteVarInt(writer, 7);
        Serialization::WriteVarInt(writer, 0);

        Buffer::Reader reader(&buff);
        REQUIRE(!ServerMessageFactory{}.Extract(reader));
    }

    SECTION("A movement count no message holds")
    {
        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        writer.WriteBits(kClientReferencesMoveRequest, 8);
        Serialization::WriteVarInt(writer, 1234);
        Serialization::WriteVarInt(writer, 1ull << 33);

        Buffer::Reader reader(&buff);
        REQUIRE(!ClientMessageFactory{}.Extract(reader));
    }

    SECTION("An animation variable count no graph has")
    {
        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        Serialization::WriteVarInt(writer, 1ull << 40);
        Serialization::WriteVarInt(writer, 0);
        Serialization::WriteVarInt(writer, 0);

        Buffer::Reader reader(&buff);
        DecodeGuard::t_failed = false;
        AnimationVariables variables;
        variables.ApplyDiff(reader);
        REQUIRE(DecodeGuard::t_failed);
        REQUIRE(variables.Booleans.empty());
        DecodeGuard::t_failed = false;
    }

    SECTION("A sound message still reads")
    {
        Buffer buff(1000);
        RequestActionEvents sendMessage;
        sendMessage.Actions[7].resize(2);
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);
        REQUIRE(ClientMessageFactory{}.Extract(reader));
    }
}

TEST_CASE("An inventory with a count no message holds is dropped", "[encoding.damaged]")
{
    Buffer buff(100);
    Buffer::Writer writer(&buff);
    Serialization::WriteVarInt(writer, 0xFFFFFFFFull);
    Buffer::Reader reader(&buff);

    Inventory inventory;
    inventory.Deserialize(reader);
    REQUIRE(DecodeGuard::t_failed);
    REQUIRE(inventory.Entries.empty());
    DecodeGuard::t_failed = false;
}

TEST_CASE("StringCache", "[encoding.string_cache]")
{
    SECTION("Messages")
    {
        StringCacheUpdate update;
        update.Values.push_back("Hello");
        update.Values.push_back("Bye");

        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        update.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        StringCacheUpdate recvUpdate;
        recvUpdate.DeserializeRaw(reader);

        REQUIRE(update == recvUpdate);
    }
}

TEST_CASE("Actor value lists read from the network are bounded", "[encoding.actor_values]")
{
    // A count passes what any real message holds, written by hand as a crafted message would be: the reader must stop at once and fail the
    // message, not loop for as many entries as the count says.
    constexpr uint64_t cHugeCount = 1ull << 40;

    SECTION("a huge count fails every reader")
    {
        {
            Buffer buff(64);
            Buffer::Writer writer(&buff);
            writer.WriteBits(RequestActorValueChanges::Opcode, 8);
            Serialization::WriteVarInt(writer, 1);
            Serialization::WriteVarInt(writer, 1);
            Serialization::WriteVarInt(writer, cHugeCount);
            Buffer::Reader reader(&buff);
            REQUIRE(!ClientMessageFactory{}.Extract(reader));
            REQUIRE(DecodeGuard::t_failed);
            DecodeGuard::t_failed = false;
        }
        {
            Buffer buff(64);
            Buffer::Writer writer(&buff);
            writer.WriteBits(RequestActorMaxValueChanges::Opcode, 8);
            Serialization::WriteVarInt(writer, 1);
            Serialization::WriteVarInt(writer, 1);
            Serialization::WriteVarInt(writer, cHugeCount);
            Buffer::Reader reader(&buff);
            REQUIRE(!ClientMessageFactory{}.Extract(reader));
            REQUIRE(DecodeGuard::t_failed);
            DecodeGuard::t_failed = false;
        }
        {
            Buffer buff(64);
            Buffer::Writer writer(&buff);
            writer.WriteBits(NotifyActorValueChanges::Opcode, 8);
            Serialization::WriteVarInt(writer, 1);
            Serialization::WriteVarInt(writer, 1);
            Serialization::WriteVarInt(writer, cHugeCount);
            Buffer::Reader reader(&buff);
            REQUIRE(!ServerMessageFactory{}.Extract(reader));
            REQUIRE(DecodeGuard::t_failed);
            DecodeGuard::t_failed = false;
        }
        {
            Buffer buff(64);
            Buffer::Writer writer(&buff);
            writer.WriteBits(NotifyActorMaxValueChanges::Opcode, 8);
            Serialization::WriteVarInt(writer, 1);
            Serialization::WriteVarInt(writer, 1);
            Serialization::WriteVarInt(writer, cHugeCount);
            Buffer::Reader reader(&buff);
            REQUIRE(!ServerMessageFactory{}.Extract(reader));
            REQUIRE(DecodeGuard::t_failed);
            DecodeGuard::t_failed = false;
        }
        {
            // A count that fits 32 bits but not the message, and one that would turn negative as an int.
            for (const uint64_t cCount : {ActorValueLimits::kMaxEntries + 1, uint64_t{1} << 31})
            {
                Buffer buff(64);
                Buffer::Writer writer(&buff);
                Serialization::WriteVarInt(writer, cCount);
                Buffer::Reader reader(&buff);
                ActorValues values;
                values.Deserialize(reader);
                REQUIRE(DecodeGuard::t_failed);
                DecodeGuard::t_failed = false;
                REQUIRE(values.ActorValuesList.empty());
            }
        }
        {
            // The second list (the maximums) is bounded as well.
            Buffer buff(64);
            Buffer::Writer writer(&buff);
            Serialization::WriteVarInt(writer, 0);
            Serialization::WriteVarInt(writer, cHugeCount);
            Buffer::Reader reader(&buff);
            ActorValues values;
            values.Deserialize(reader);
            REQUIRE(DecodeGuard::t_failed);
            DecodeGuard::t_failed = false;
            REQUIRE(values.ActorMaxValuesList.empty());
        }
    }

    SECTION("real lists pass, and a key no actor value has is left out")
    {
        RequestActorValueChanges sent;
        sent.Id = 7;
        sent.OwnershipEpoch = 3;
        sent.Values = {{0, 1.f}, {5, 2.5f}, {static_cast<uint32_t>(ActorValueLimits::kKeyLimit - 1), 9.f}, {static_cast<uint32_t>(ActorValueLimits::kKeyLimit), 4.f}, {5000, 6.f}};
        Buffer buff(512);
        Buffer::Writer writer(&buff);
        sent.Serialize(writer);
        Buffer::Reader reader(&buff);
        const auto pBack = ClientMessageFactory{}.Extract(reader);
        REQUIRE(pBack);
        REQUIRE(!DecodeGuard::t_failed);
        const auto* pValues = static_cast<RequestActorValueChanges*>(pBack.get());
        REQUIRE(pValues->Id == 7);
        REQUIRE(pValues->OwnershipEpoch == 3);
        REQUIRE(pValues->Values.size() == 3);
        REQUIRE(pValues->Values.count(0) == 1);
        REQUIRE(pValues->Values.count(5) == 1);
        REQUIRE(pValues->Values.count(static_cast<uint32_t>(ActorValueLimits::kKeyLimit - 1)) == 1);
        REQUIRE(pValues->Values.count(static_cast<uint32_t>(ActorValueLimits::kKeyLimit)) == 0);
        REQUIRE(pValues->Values.count(5000) == 0);
    }

    SECTION("a key past 32 bits is not folded onto a real key")
    {
        // 2^32 + 5 would read as key 5 if it were cut to 32 bits before the test.
        Buffer buff(64);
        Buffer::Writer writer(&buff);
        writer.WriteBits(NotifyActorValueChanges::Opcode, 8);
        Serialization::WriteVarInt(writer, 1);
        Serialization::WriteVarInt(writer, 1);
        Serialization::WriteVarInt(writer, 1);
        Serialization::WriteVarInt(writer, (uint64_t{1} << 32) + 5);
        Serialization::WriteFloat(writer, 1.f);
        Buffer::Reader reader(&buff);
        const auto pBack = ServerMessageFactory{}.Extract(reader);
        REQUIRE(pBack);
        REQUIRE(static_cast<NotifyActorValueChanges*>(pBack.get())->Values.empty());
    }

    SECTION("a character's own lists round trip")
    {
        ActorValues sent;
        sent.ActorValuesList = {{1, 10.f}, {static_cast<uint32_t>(ActorValueLimits::kKeyLimit - 1), 3.f}};
        sent.ActorMaxValuesList = {{2, 100.f}};
        Buffer buff(512);
        Buffer::Writer writer(&buff);
        sent.Serialize(writer);
        Buffer::Reader reader(&buff);
        ActorValues back;
        back.Deserialize(reader);
        REQUIRE(!DecodeGuard::t_failed);
        REQUIRE(back == sent);
    }
}

TEST_CASE("A flag string shorter than its flags is not read past its end", "[encoding.animvars_short]")
{
    // The flags need a bit each; a string from the network may hold fewer bytes than that.
    AnimationVariables vars;
    TiltedPhoques::Vector<bool> flags(384, true);
    const TiltedPhoques::String shortString("\xFF\xFF", 2);

    vars.String_to_VectorBool(shortString, flags);

    // Sixteen flags came from the two bytes; the rest stay false.
    for (size_t i = 0; i < flags.size(); ++i)
        REQUIRE(flags[i] == (i < 16));

    // The empty string sets nothing.
    vars.String_to_VectorBool(TiltedPhoques::String{}, flags);
    for (size_t i = 0; i < flags.size(); ++i)
        REQUIRE(!flags[i]);
}

TEST_CASE("A flag diff with a short string is dropped whole", "[encoding.animvars_short]")
{
    // A count of 200 flags needs 25 bytes of flags; this message sends 2.
    Buffer buff(256);
    Buffer::Writer writer(&buff);
    Serialization::WriteVarInt(writer, 200);
    Serialization::WriteVarInt(writer, 0);
    Serialization::WriteVarInt(writer, 0);
    Serialization::WriteString(writer, TiltedPhoques::String("\xFF\xFF", 2));
    Buffer::Reader reader(&buff);

    AnimationVariables vars;
    vars.ApplyDiff(reader);

    REQUIRE(DecodeGuard::t_failed);
    DecodeGuard::t_failed = false;
    REQUIRE(vars.Booleans.empty());
}

TEST_CASE("A faction count no message holds fails the message and does not throw", "[encoding.damaged]")
{
    // The readers cannot throw: an exception out of one ends the server.
    for (const uint64_t cCount : {uint64_t{0x200}, uint64_t{1} << 40})
    {
        {
            Buffer buff(64);
            Buffer::Writer writer(&buff);
            Serialization::WriteVarInt(writer, cCount);
            Buffer::Reader reader(&buff);
            Factions factions;
            REQUIRE_NOTHROW(factions.Deserialize(reader));
            REQUIRE(DecodeGuard::t_failed);
            DecodeGuard::t_failed = false;
        }
        {
            // The count of the second list.
            Buffer buff(64);
            Buffer::Writer writer(&buff);
            Serialization::WriteVarInt(writer, 0);
            Serialization::WriteVarInt(writer, cCount);
            Buffer::Reader reader(&buff);
            Factions factions;
            REQUIRE_NOTHROW(factions.Deserialize(reader));
            REQUIRE(DecodeGuard::t_failed);
            DecodeGuard::t_failed = false;
        }
    }

    // Through a message, as a crafted one arrives.
    Buffer buff(64);
    Buffer::Writer writer(&buff);
    writer.WriteBits(kRequestFactionsChanges, 8);
    writer.WriteBits(1, 8);
    Serialization::WriteVarInt(writer, 5);
    Serialization::WriteVarInt(writer, 0x200);
    Buffer::Reader reader(&buff);
    REQUIRE(!ClientMessageFactory{}.Extract(reader));
    DecodeGuard::t_failed = false;
}

TEST_CASE("Action counts that add up past 64 bits are dropped", "[encoding.damaged]")
{
    // Actor 1 holds one action and actor 2 holds 2^64 - 1: the sum wraps to zero, and a sum that is checked alone would let the resize through.
    constexpr uint64_t cWrap = ~uint64_t{0};
    {
        Buffer buff(100);
        Buffer::Writer writer(&buff);
        writer.WriteBits(kRequestActionEvents, 8);
        Serialization::WriteVarInt(writer, 2);
        Serialization::WriteVarInt(writer, 1);
        Serialization::WriteVarInt(writer, 1);
        ActionEvent{}.GenerateDifferential(ActionEvent{}, writer);
        Serialization::WriteVarInt(writer, 2);
        Serialization::WriteVarInt(writer, cWrap);
        Buffer::Reader reader(&buff);
        REQUIRE(!ClientMessageFactory{}.Extract(reader));
    }
    {
        Buffer buff(100);
        Buffer::Writer writer(&buff);
        writer.WriteBits(kNotifyActionEvents, 8);
        Serialization::WriteVarInt(writer, 2);
        Serialization::WriteVarInt(writer, 1);
        Serialization::WriteVarInt(writer, 1);
        ActionEvent{}.GenerateDifferential(ActionEvent{}, writer);
        Serialization::WriteVarInt(writer, 2);
        Serialization::WriteVarInt(writer, cWrap);
        Buffer::Reader reader(&buff);
        REQUIRE(!ServerMessageFactory{}.Extract(reader));
    }
}

TEST_CASE("A string length is checked before anything is allocated", "[encoding.bounded_read]")
{
    DecodeGuard::t_failed = false;

    SECTION("a string the buffer holds is read")
    {
        Buffer buff(64);
        Buffer::Writer writer(&buff);
        Serialization::WriteString(writer, TiltedPhoques::String("Skyrim.esm"));
        Serialization::WriteString(writer, TiltedPhoques::String());
        Buffer::Reader reader(&buff);
        REQUIRE(BoundedString::Read(reader, 256) == "Skyrim.esm");
        REQUIRE(BoundedString::Read(reader, 256).empty());
        REQUIRE(!DecodeGuard::t_failed);
    }

    SECTION("an empty string in the middle leaves the next one readable")
    {
        // The writer starts its bytes on a byte boundary even for none, after a bit-sized field in front.
        Buffer buff(64);
        Buffer::Writer writer(&buff);
        Serialization::WriteBool(writer, true);
        Serialization::WriteString(writer, TiltedPhoques::String());
        Serialization::WriteString(writer, TiltedPhoques::String("after"));
        Buffer::Reader reader(&buff);
        REQUIRE(Serialization::ReadBool(reader));
        REQUIRE(BoundedString::Read(reader, 256).empty());
        REQUIRE(BoundedString::Read(reader, 256) == "after");
        REQUIRE(!DecodeGuard::t_failed);
    }

    SECTION("a length over the limit, or over the bytes left, fails")
    {
        Buffer buff(64);
        Buffer::Writer writer(&buff);
        Serialization::WriteVarInt(writer, 60000);
        Buffer::Reader reader(&buff);
        REQUIRE(BoundedString::Read(reader, 256).empty());
        REQUIRE(DecodeGuard::t_failed);
        DecodeGuard::t_failed = false;

        Buffer small(64);
        Buffer::Writer smallWriter(&small);
        Serialization::WriteVarInt(smallWriter, 100);
        Buffer::Reader smallReader(&small);
        REQUIRE(BoundedString::Read(smallReader, 256).empty());
        REQUIRE(DecodeGuard::t_failed);
        DecodeGuard::t_failed = false;
    }
}

TEST_CASE("A login with an oversized text or mod list is dropped", "[encoding.damaged]")
{
    SECTION("a real login reads")
    {
        AuthenticationRequest sent;
        sent.Token = "secret";
        sent.Version = "abc123";
        sent.Protocol = "12345678";
        sent.Username = "Dovahkiin";
        Mods::Entry entry;
        entry.Filename = "Skyrim.esm";
        entry.Id = 1;
        sent.UserMods.ModList.push_back(entry);
        Buffer buff(1024);
        Buffer::Writer writer(&buff);
        sent.Serialize(writer);
        Buffer::Reader reader(&buff);
        const auto pBack = ClientMessageFactory{}.Extract(reader);
        REQUIRE(pBack);
        const auto* pLogin = static_cast<AuthenticationRequest*>(pBack.get());
        REQUIRE(pLogin->Token == "secret");
        REQUIRE(pLogin->Username == "Dovahkiin");
        REQUIRE(pLogin->UserMods.ModList.size() == 1);
        REQUIRE(pLogin->UserMods.ModList[0].Filename == "Skyrim.esm");
    }

    SECTION("a mod name claiming 60000 bytes in a short message fails before it allocates")
    {
        Buffer buff(64);
        Buffer::Writer writer(&buff);
        writer.WriteBits(2, 13);
        writer.WriteBits(1, 16);
        Serialization::WriteBool(writer, false);
        Serialization::WriteVarInt(writer, 60000);
        Buffer::Reader reader(&buff);
        Mods mods;
        mods.Deserialize(reader);
        REQUIRE(DecodeGuard::t_failed);
        DecodeGuard::t_failed = false;
        REQUIRE(mods.ModList.empty());
    }

    SECTION("more mods than a list holds fails")
    {
        Buffer buff(64);
        Buffer::Writer writer(&buff);
        writer.WriteBits(8191, 13);
        Buffer::Reader reader(&buff);
        Mods mods;
        mods.Deserialize(reader);
        REQUIRE(DecodeGuard::t_failed);
        DecodeGuard::t_failed = false;
    }

    SECTION("a token of 60000 bytes fails")
    {
        Buffer buff(64);
        Buffer::Writer writer(&buff);
        writer.WriteBits(kAuthenticationRequest, 8);
        Serialization::WriteVarInt(writer, 0);
        Serialization::WriteBool(writer, false);
        Serialization::WriteBool(writer, false);
        Serialization::WriteVarInt(writer, 60000);
        Buffer::Reader reader(&buff);
        REQUIRE(!ClientMessageFactory{}.Extract(reader));
        DecodeGuard::t_failed = false;
    }
}
