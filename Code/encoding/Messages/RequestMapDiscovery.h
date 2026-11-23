#pragma once

#include "Message.h"

#include <Structs/MapDiscovery.h>

//! Map markers this player discovered since the last send, for the party to discover too.
struct RequestMapDiscovery final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestMapDiscovery;

    RequestMapDiscovery()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { MapDiscoveryList::Write(aWriter, Discoveries); }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ClientMessage::DeserializeRaw(aReader);
        MapDiscoveryList::Read(aReader, Discoveries);
    }

    bool operator==(const RequestMapDiscovery& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Discoveries == acRhs.Discoveries; }

    TiltedPhoques::Vector<MapDiscovery> Discoveries{};
};
