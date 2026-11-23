#pragma once

#include "Message.h"

#include <Structs/MapDiscovery.h>

//! Map markers a party member discovered: as they happen, and the party's whole list on joining it.
struct NotifyMapDiscovery final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyMapDiscovery;

    NotifyMapDiscovery()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { MapDiscoveryList::Write(aWriter, Discoveries); }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ServerMessage::DeserializeRaw(aReader);
        MapDiscoveryList::Read(aReader, Discoveries);
    }

    bool operator==(const NotifyMapDiscovery& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Discoveries == acRhs.Discoveries; }

    TiltedPhoques::Vector<MapDiscovery> Discoveries{};
};
