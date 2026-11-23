#include <Messages/RequestPartyValue.h>
#include <BoundedString.h>

void RequestPartyValue::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteString(aWriter, Key);
    Serialization::WriteFloat(aWriter, Value);
}

void RequestPartyValue::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Key = BoundedString::Read(aReader, RequestPartyValue::kLongestKey);
    Value = Serialization::ReadFloat(aReader);
}
