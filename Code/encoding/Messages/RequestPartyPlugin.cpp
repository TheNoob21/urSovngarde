#include <Messages/RequestPartyPlugin.h>
#include <BoundedString.h>

void RequestPartyPlugin::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteString(aWriter, PluginFile);
}

void RequestPartyPlugin::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    PluginFile = BoundedString::Read(aReader, 260);
}
