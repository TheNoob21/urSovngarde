#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <TiltedCore/ViewBuffer.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <catch2/catch.hpp>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Systems/ExtensionHandlers.h>

using namespace TiltedPhoques;

TEST_CASE("Extension packets pass through both message factories", "[encoding.extension]")
{
    RequestExtension request;
    request.Data.Addon = 123;
    request.Data.Version = 5;
    request.Data.Scope = UR_EXTENSION_OWNER;
    request.Data.Target = 75;
    request.Data.TargetEpoch = 7;
    request.Data.Bytes.resize(UR_EXTENSION_MAX_BYTES, 0xda);
    Buffer buffer(UR_EXTENSION_MAX_BYTES + 512);
    Buffer::Writer writer(&buffer);
    request.Serialize(writer);
    Buffer::Reader reader(&buffer);
    auto decoded = ClientMessageFactory{}.Extract(reader);
    REQUIRE(decoded);
    const auto& data = static_cast<RequestExtension&>(*decoded).Data;
    REQUIRE(data.Version == 5);
    REQUIRE(data.Target == 75);
    REQUIRE(data.Bytes == request.Data.Bytes);

    NotifyExtension notify;
    notify.Sender = 2;
    notify.Data = request.Data;
    notify.Data.Subject = 15;
    notify.Data.SubjectEpoch = 1;
    Buffer reply(UR_EXTENSION_MAX_BYTES + 512);
    Buffer::Writer replyWriter(&reply);
    notify.Serialize(replyWriter);
    Buffer::Reader replyReader(&reply);
    auto response = ServerMessageFactory{}.Extract(replyReader);
    REQUIRE(response);
    REQUIRE(static_cast<NotifyExtension&>(*response).Sender == 2);
    REQUIRE(static_cast<NotifyExtension&>(*response).Data.Bytes == request.Data.Bytes);

    request.Data.Bytes.push_back(1);
    Buffer oversized(UR_EXTENSION_MAX_BYTES + 512);
    Buffer::Writer oversizedWriter(&oversized);
    request.Serialize(oversizedWriter);
    Buffer::Reader oversizedReader(&oversized);
    REQUIRE_FALSE(ClientMessageFactory{}.Extract(oversizedReader));
}

TEST_CASE("Only the registered extension receives its messages", "[client.extension]")
{
    static uint32_t sender;
    sender = 0;
    ExtensionHandlers handlers;
    UrExtensionHandler handler{[](uint32_t aSender, const UrExtensionMessage*)
                               {
                                   sender = aSender;
                               }};
    REQUIRE(handlers.Register(1, &handler));
    REQUIRE_FALSE(handlers.Register(1, &handler));
    REQUIRE_FALSE(handlers.Register(0, &handler));
    UrExtensionMessage message{};
    message.Addon = 2;
    handlers.Receive(14, message);
    REQUIRE(sender == 0);
    message.Addon = 1;
    handlers.Receive(15, message);
    REQUIRE(sender == 15);
}

TEST_CASE("An extension request cannot impersonate an acknowledgement or publish a private key", "[encoding.extension]")
{
    ExtensionData data;
    data.Addon = 123;
    data.Version = 1;
    data.Key = 42;
    data.State = UR_EXTENSION_STORE;
    data.Bytes = {8};
    CHECK_FALSE(data.ValidRequest());
    data.WriterLow = 953;
    REQUIRE(data.ValidRequest());
    CHECK_FALSE(data.ValidNotification(0));
    data.Result = 1;
    CHECK_FALSE(data.ValidRequest());
    data.WriterLow = 0;
    REQUIRE(data.ValidNotification(0));
    data.State = UR_EXTENSION_READ;
    data.Result = 0;
    data.WriterHigh = 876;
    CHECK_FALSE(data.ValidRequest());
    data.Bytes.clear();
    REQUIRE(data.ValidRequest());

    ExtensionData rejected;
    rejected.Addon = 123;
    rejected.Version = 1;
    rejected.State = UR_EXTENSION_CLAIM;
    rejected.Scope = UR_EXTENSION_OWNER;
    rejected.Subject = 2;
    rejected.Target = 3;
    rejected.TargetEpoch = rejected.SubjectEpoch = 1;
    rejected.Key = 77;
    rejected.Result = 2;
    REQUIRE_FALSE(rejected.ValidRequest());
    REQUIRE(rejected.ValidNotification(4));
    rejected.Bytes = {1};
    REQUIRE_FALSE(rejected.ValidNotification(4));

    RequestExtension request;
    request.Data = data;
    Buffer buffer(256);
    Buffer::Writer writer(&buffer);
    request.Serialize(writer);
    for (size_t size = 1; size < writer.Size(); ++size)
    {
        ViewBuffer shortPacket(reinterpret_cast<uint8_t*>(buffer.GetWriteData()), size);
        Buffer::Reader reader(&shortPacket);
        REQUIRE_FALSE(ClientMessageFactory{}.Extract(reader));
    }
}
