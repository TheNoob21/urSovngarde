#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <optional>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <DecodeGuard.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>

#include <catch2/catch.hpp>

using namespace TiltedPhoques;

TEST_CASE("Authentication carries plugin sizes in the existing mod order", "[mod_content][encoding]")
{
    AuthenticationRequest request{};
    request.UserMods.ModList = {{"Skyrim.esm", 0, false}, {"City.esl", 42, true}, {"PlayerMenu.esp", 1, false}};
    request.ContentSizes.GameEdition = ModContentSizes::Edition::kVR;
    request.ContentSizes.Sizes = {250'000'000, 0x100001245ull, 0};

    uint8_t storage[4096]{};
    Buffer buffer(storage, sizeof(storage));
    Buffer::Writer writer(&buffer);
    request.Serialize(writer);
    Buffer::Reader reader(&buffer);
    auto pMessage = ClientMessageFactory{}.Extract(reader);
    REQUIRE(pMessage);
    CHECK(*CastUnique<AuthenticationRequest>(std::move(pMessage)) == request);
}

TEST_CASE("Authentication carries the shared content difference on refusal and warning", "[mod_content][encoding]")
{
    using RT = AuthenticationResponse::ResponseType;
    AuthenticationResponse response{};
    response.Type = GENERATE(RT::kModsMismatch, RT::kAccepted);
    response.ContentDifference.Incomplete = true;
    response.ContentDifference.Truncated = true;
    response.ContentDifference.Missing = {"Missing.esm"};
    response.ContentDifference.Extra = {"Extra.esp"};
    response.ContentDifference.Different = {"City.esl"};
    response.ContentDifference.Unreadable = {"Unreadable.esm"};

    uint8_t storage[4096]{};
    Buffer buffer(storage, sizeof(storage));
    Buffer::Writer writer(&buffer);
    response.Serialize(writer);
    Buffer::Reader reader(&buffer);
    auto pMessage = ServerMessageFactory{}.Extract(reader);
    REQUIRE(pMessage);
    CHECK(*CastUnique<AuthenticationResponse>(std::move(pMessage)) == response);
}

TEST_CASE("Authentication rejects a plugin size array beyond the mod count bound", "[mod_content][encoding]")
{
    uint8_t storage[64]{};
    Buffer buffer(storage, sizeof(storage));
    Buffer::Writer writer(&buffer);
    writer.WriteBits(static_cast<uint8_t>(ModContentSizes::Edition::kSE), 2);
    Serialization::WriteVarInt(writer, 4097);

    Buffer::Reader reader(&buffer);
    ModContentSizes sizes;
    DecodeGuard::t_failed = false;
    sizes.Deserialize(reader);
    const auto cRejected = DecodeGuard::t_failed;
    DecodeGuard::t_failed = false;
    CHECK(cRejected);
    CHECK(sizes.Sizes.empty());
}
