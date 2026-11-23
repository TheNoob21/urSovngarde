#include <Messages/TeleportCommandRequest.h>
#include <BoundedString.h>

void TeleportCommandRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteString(aWriter, TargetPlayer);
}

void TeleportCommandRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    TargetPlayer = BoundedString::Read(aReader, 256);
}
