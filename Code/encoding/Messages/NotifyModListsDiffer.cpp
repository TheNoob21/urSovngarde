#include <Messages/NotifyModListsDiffer.h>
#include <Structs/PluginList.h>

void NotifyModListsDiffer::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteString(aWriter, Username);
    PluginList::Write(aWriter, OnlyHere);
    PluginList::Write(aWriter, OnlyThere);
}

void NotifyModListsDiffer::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    Username = Serialization::ReadString(aReader);
    PluginList::Read(aReader, OnlyHere);
    PluginList::Read(aReader, OnlyThere);
}
