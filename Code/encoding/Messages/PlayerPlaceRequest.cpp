#include <Messages/PlayerPlaceRequest.h>
#include <BoundedString.h>

void PlayerPlaceRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteString(aWriter, Place);
}

void PlayerPlaceRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Place = BoundedString::Read(aReader, 256);
}
