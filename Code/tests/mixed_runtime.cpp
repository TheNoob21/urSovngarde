#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>

#include <catch2/catch.hpp>
#include <Messages/AuthenticationRequest.h>
#include <Messages/AuthenticationResponse.h>
#include <Messages/ClientReferencesMoveRequest.h>
#include <Messages/ServerReferencesMoveRequest.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>

using TiltedPhoques::Buffer;

// Synthetic packet fixtures: these exercise shared encoding, not server admission,
// the server's mapping algorithm, or actual SE/VR executables.
TEST_CASE("Two synthetic clients retain authentication and mod mapping fields", "[mixed.runtime]")
{
    AuthenticationRequest se, vr;
    se.Version = "se-build";
    vr.Version = "vr-build";
    se.Protocol = vr.Protocol = "shared-protocol";
    se.Username = "se";
    vr.Username = "vr";
    se.UserMods.ModList = {{"Skyrim.esm", 0, false}, {"Shared.esp", 2, false}, {"Light.esl", 3, true}};
    vr.UserMods.ModList = {{"Skyrim.esm", 0, false}, {"Shared.esp", 7, false}, {"Light.esl", 9, true}};

    for (const auto* request : {&se, &vr})
    {
        Buffer bytes(4096);
        Buffer::Writer writer(&bytes);
        request->Serialize(writer);
        Buffer::Reader reader(&bytes);
        auto message = ClientMessageFactory{}.Extract(reader);
        REQUIRE(message);
        REQUIRE(message->GetOpcode() == AuthenticationRequest::Opcode);
        const auto decoded = TiltedPhoques::CastUnique<AuthenticationRequest>(std::move(message));
        CHECK(decoded->Version == request->Version);
        CHECK(decoded->Protocol == request->Protocol);
        CHECK(decoded->UserMods == request->UserMods);
    }

    // These are supplied server response examples, not an emulation of ModsComponent.
    AuthenticationResponse reply;
    reply.Type = AuthenticationResponse::ResponseType::kAccepted;
    reply.UserMods.ModList = {{"Skyrim.esm", 0, false}, {"Shared.esp", 11, false}, {"Light.esl", 12, true}};
    Buffer bytes(4096);
    Buffer::Writer writer(&bytes);
    reply.Serialize(writer);
    Buffer::Reader reader(&bytes);
    auto message = ServerMessageFactory{}.Extract(reader);
    REQUIRE(message);
    REQUIRE(message->GetOpcode() == AuthenticationResponse::Opcode);
    const auto decoded = TiltedPhoques::CastUnique<AuthenticationResponse>(std::move(message));
    CHECK(decoded->Type == AuthenticationResponse::ResponseType::kAccepted);
    CHECK(decoded->UserMods == reply.UserMods);
}

TEST_CASE("Synthetic movement preserves VR and non-VR snapshots across both packet directions", "[mixed.runtime]")
{
    ClientReferencesMoveRequest client;
    client.Tick = 42;
    ReferenceUpdate se, vr;
    se.UpdatedMovement.WorldSpaceId = GameId(11, 0x1234);
    vr.UpdatedMovement.WorldSpaceId = GameId(11, 0x1234);
    vr.UpdatedVRPose.HasData = true;
    vr.UpdatedVRPose.NoBones = true;
    vr.UpdatedVRPose.HasHips = true;
    vr.UpdatedVRPose.HipOffset[0] = 1.f;
    client.Updates.emplace(1, se);
    client.Updates.emplace(2, vr);
    Buffer incoming(4096);
    Buffer::Writer clientWriter(&incoming);
    client.Serialize(clientWriter);
    Buffer::Reader clientReader(&incoming);
    auto clientMessage = ClientMessageFactory{}.Extract(clientReader);
    REQUIRE(clientMessage);
    REQUIRE(clientMessage->GetOpcode() == ClientReferencesMoveRequest::Opcode);
    const auto received = TiltedPhoques::CastUnique<ClientReferencesMoveRequest>(std::move(clientMessage));
    REQUIRE(received->Updates.size() == 2);
    CHECK_FALSE(received->Updates.at(1).UpdatedVRPose.HasData);
    CHECK(received->Updates.at(2).UpdatedVRPose.HipOffset[0] == 1.f);
    CHECK(received->Updates.at(2).UpdatedMovement.WorldSpaceId == GameId(11, 0x1234));

    // Explicitly construct a relay fixture; no server routing is simulated here.
    ServerReferencesMoveRequest relay;
    relay.Tick = received->Tick;
    relay.Updates = received->Updates;
    Buffer outgoing(4096);
    Buffer::Writer serverWriter(&outgoing);
    relay.Serialize(serverWriter);
    Buffer::Reader serverReader(&outgoing);
    auto serverMessage = ServerMessageFactory{}.Extract(serverReader);
    REQUIRE(serverMessage);
    REQUIRE(serverMessage->GetOpcode() == ServerReferencesMoveRequest::Opcode);
    const auto delivered = TiltedPhoques::CastUnique<ServerReferencesMoveRequest>(std::move(serverMessage));
    REQUIRE(delivered->Updates.size() == 2);
    CHECK_FALSE(delivered->Updates.at(1).UpdatedVRPose.HasData);
    CHECK(delivered->Updates.at(2).UpdatedVRPose.HasData);
    CHECK(delivered->Updates.at(2).UpdatedVRPose.HipOffset[0] == 1.f);
    CHECK(delivered->Updates.at(2).UpdatedMovement.WorldSpaceId == GameId(11, 0x1234));
}
