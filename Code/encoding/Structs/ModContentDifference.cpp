#include <Structs/ModContentDifference.h>

#include <Structs/PluginList.h>

using TiltedPhoques::Serialization;

void ModContentDifference::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteBool(aWriter, Incomplete);
    Serialization::WriteBool(aWriter, Truncated);
    PluginList::Write(aWriter, Missing);
    PluginList::Write(aWriter, Extra);
    PluginList::Write(aWriter, Different);
    PluginList::Write(aWriter, Unreadable);
}

void ModContentDifference::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Incomplete = Serialization::ReadBool(aReader);
    Truncated = Serialization::ReadBool(aReader);
    PluginList::Read(aReader, Missing);
    PluginList::Read(aReader, Extra);
    PluginList::Read(aReader, Different);
    PluginList::Read(aReader, Unreadable);
}
