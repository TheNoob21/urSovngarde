#include <catch2/catch.hpp>

#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <Messages/NotifyServerIdentity.h>

TEST_CASE("server identity carries the configured name and connected player count", "[encoding][co-op]")
{
    NotifyServerIdentity message;
    message.Name = "High Hrothgar Co-op";
    message.Players = 24;

    TiltedPhoques::Buffer buffer(512);
    TiltedPhoques::Buffer::Writer writer(&buffer);
    message.SerializeRaw(writer);

    TiltedPhoques::Buffer::Reader reader(&buffer);
    NotifyServerIdentity decoded;
    decoded.DeserializeRaw(reader);

    CHECK(decoded == message);
}
