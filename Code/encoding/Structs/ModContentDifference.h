#pragma once

#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Stl.hpp>

struct ModContentDifference
{
    bool Incomplete{};
    bool Truncated{};
    TiltedPhoques::Vector<TiltedPhoques::String> Missing;
    TiltedPhoques::Vector<TiltedPhoques::String> Extra;
    TiltedPhoques::Vector<TiltedPhoques::String> Different;
    TiltedPhoques::Vector<TiltedPhoques::String> Unreadable;

    bool Empty() const noexcept { return !Incomplete && !Truncated && Missing.empty() && Extra.empty() && Different.empty() && Unreadable.empty(); }
    bool operator==(const ModContentDifference&) const noexcept = default;
    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
};
