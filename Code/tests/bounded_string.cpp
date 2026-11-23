#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <catch2/catch.hpp>

#include <BoundedString.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/SendChatMessageRequest.h>
#include <Messages/RequestPartyValue.h>

using namespace TiltedPhoques;

TEST_CASE("a string written by the library reads back the same", "[boundedstring]")
{
    DecodeGuard::t_failed = false;
    Buffer buffer(1 << 12);
    Buffer::Writer writer(&buffer);
    Serialization::WriteString(writer, "Whiterun Hold");
    Serialization::WriteString(writer, "");
    Serialization::WriteString(writer, String(300, 'x'));

    Buffer::Reader reader(&buffer);
    CHECK(BoundedString::Read(reader, 64) == "Whiterun Hold");
    CHECK(BoundedString::Read(reader, 64).empty());
    CHECK(BoundedString::Read(reader, 512) == String(300, 'x'));
    CHECK_FALSE(DecodeGuard::t_failed);
}

TEST_CASE("a string longer than its limit fails the message", "[boundedstring]")
{
    DecodeGuard::t_failed = false;
    Buffer buffer(1 << 12);
    Buffer::Writer writer(&buffer);
    Serialization::WriteString(writer, String(100, 'y'));

    Buffer::Reader reader(&buffer);
    CHECK(BoundedString::Read(reader, 99).empty());
    CHECK(DecodeGuard::t_failed);
    DecodeGuard::t_failed = false;
}

TEST_CASE("a string that claims more bytes than the message has fails without reading them", "[boundedstring]")
{
    DecodeGuard::t_failed = false;
    // A length of 60000 bytes, and only a few after it: the library's reader would make a 60000-byte string first.
    Buffer buffer(16);
    Buffer::Writer writer(&buffer);
    Serialization::WriteVarInt(writer, 60000);
    Serialization::WriteVarInt(writer, 1);
    Serialization::WriteVarInt(writer, 2);

    Buffer::Reader reader(&buffer);
    CHECK(BoundedString::Read(reader, 65535).empty());
    CHECK(DecodeGuard::t_failed);
    DecodeGuard::t_failed = false;
}

TEST_CASE("a string as long as the bytes left is read whole", "[boundedstring]")
{
    DecodeGuard::t_failed = false;
    Buffer buffer(6);
    Buffer::Writer writer(&buffer);
    Serialization::WriteString(writer, "abcd");

    Buffer::Reader reader(&buffer);
    CHECK(BoundedString::Read(reader, 16) == "abcd");
    CHECK_FALSE(DecodeGuard::t_failed);
}

TEST_CASE("a chat message from a game is read, an oversize one fails the message", "[boundedstring]")
{
    for (const size_t length : {size_t{1}, size_t{200}, static_cast<size_t>(BoundedString::kLongest), static_cast<size_t>(BoundedString::kLongest) + 1, size_t{60000}})
    {
        DecodeGuard::t_failed = false;
        SendChatMessageRequest sent;
        sent.MessageType = ChatMessageType::kGlobalChat;
        sent.ChatMessage = String(length, 'a');

        Buffer buffer(1 << 17);
        Buffer::Writer writer(&buffer);
        sent.Serialize(writer);
        Buffer::Reader reader(&buffer);
        const auto pMessage = ClientMessageFactory{}.Extract(reader);
        if (length <= BoundedString::kLongest)
            CHECK(pMessage);
        else
            CHECK_FALSE(pMessage);
        CHECK(DecodeGuard::t_failed == (length > BoundedString::kLongest));
    }
    DecodeGuard::t_failed = false;
}

TEST_CASE("a party value key beyond the longest the server takes fails the message", "[boundedstring]")
{
    DecodeGuard::t_failed = false;
    RequestPartyValue sent;
    sent.Key = String(RequestPartyValue::kLongestKey + 1, 'k');
    sent.Value = 1.f;

    Buffer buffer(1 << 12);
    Buffer::Writer writer(&buffer);
    sent.Serialize(writer);
    Buffer::Reader reader(&buffer);
    CHECK_FALSE(ClientMessageFactory{}.Extract(reader));
    CHECK(DecodeGuard::t_failed);
    DecodeGuard::t_failed = false;
}

TEST_CASE("an empty string leaves the reader where the next field starts", "[boundedstring]")
{
    DecodeGuard::t_failed = false;
    Buffer buffer(1 << 12);
    Buffer::Writer writer(&buffer);
    Serialization::WriteString(writer, "");
    Serialization::WriteString(writer, "after");
    Serialization::WriteBool(writer, true);
    Serialization::WriteString(writer, "");
    Serialization::WriteVarInt(writer, 77);

    Buffer::Reader reader(&buffer);
    CHECK(BoundedString::Read(reader, 64).empty());
    CHECK(BoundedString::Read(reader, 64) == "after");
    CHECK(Serialization::ReadBool(reader));
    CHECK(BoundedString::Read(reader, 64).empty());
    CHECK(Serialization::ReadVarInt(reader) == 77);
    CHECK_FALSE(DecodeGuard::t_failed);
}
