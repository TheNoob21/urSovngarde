#include <Structs/ItemDataChange.h>
#include <TiltedCore/Serialization.hpp>

void ItemDataChange::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Old.Serialize(aWriter);
    New.Serialize(aWriter);
}

void ItemDataChange::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Old.Deserialize(aReader);
    New.Deserialize(aReader);
}
