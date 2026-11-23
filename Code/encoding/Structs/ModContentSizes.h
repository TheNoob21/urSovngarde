#pragma once

#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Stl.hpp>

// File sizes, in AuthenticationRequest::UserMods order. Zero means the file could not be measured.
struct ModContentSizes
{
    enum class Edition : uint8_t
    {
        kUnknown,
        kSE,
        kVR
    };

    Edition GameEdition{};
    TiltedPhoques::Vector<uint64_t> Sizes;

    bool operator==(const ModContentSizes&) const noexcept = default;
    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
};
