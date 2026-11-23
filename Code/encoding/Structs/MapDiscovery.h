#pragma once

#include <Structs/GameId.h>
#include <DecodeGuard.h>

#include <TiltedCore/Serialization.hpp>

//! A map marker a party member discovered: on the map, and a fast travel destination when CanTravel.
struct MapDiscovery
{
    GameId MarkerId{};
    bool CanTravel{};

    bool operator==(const MapDiscovery& acRhs) const noexcept { return MarkerId == acRhs.MarkerId && CanTravel == acRhs.CanTravel; }
};

namespace MapDiscoveryList
{
//! A party's whole list goes to a player who joins it; the base game and its add-ons have far fewer markers.
constexpr uint64_t kMax = 8192;

inline void Write(TiltedPhoques::Buffer::Writer& aWriter, const TiltedPhoques::Vector<MapDiscovery>& acDiscoveries) noexcept
{
    Serialization::WriteVarInt(aWriter, acDiscoveries.size());
    for (const auto& discovery : acDiscoveries)
    {
        discovery.MarkerId.Serialize(aWriter);
        Serialization::WriteBool(aWriter, discovery.CanTravel);
    }
}

inline void Read(TiltedPhoques::Buffer::Reader& aReader, TiltedPhoques::Vector<MapDiscovery>& aDiscoveries) noexcept
{
    const auto cCount = Serialization::ReadVarInt(aReader);
    if (!DecodeGuard::Within(cCount, kMax))
        return;
    aDiscoveries.resize(static_cast<size_t>(cCount));
    for (auto& discovery : aDiscoveries)
    {
        discovery.MarkerId.Deserialize(aReader);
        discovery.CanTravel = Serialization::ReadBool(aReader);
    }
}
} // namespace MapDiscoveryList
