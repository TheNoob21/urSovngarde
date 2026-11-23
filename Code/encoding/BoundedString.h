#pragma once

#include <cstdint>

#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <TiltedCore/Stl.hpp>

#include <DecodeGuard.h>

//! Reads a string written by Serialization::WriteString, refusing one that claims more than a message can hold.
//! The library's own reader makes the string as long as it claims (up to 64 KiB) before it looks whether the bytes are there,
//! so a short message could cost a large allocation for each string in it.
namespace BoundedString
{
//! The most any string is read as, when no field-specific limit is given.
constexpr uint64_t kLongest = 4096;
//! What a string of any length a message can carry is read as: the bytes left still decide.
constexpr uint64_t kLargest = 65535;

//! The string, or an empty one (and the message failed) when it claims more than aMax bytes or more than the message has left.
inline TiltedPhoques::String Read(TiltedPhoques::Buffer::Reader& aReader, const uint64_t aMax = kLongest) noexcept
{
    using TiltedPhoques::Serialization;

    const uint64_t cLength = Serialization::ReadVarInt(aReader);
    const TiltedPhoques::Buffer* pBuffer = aReader.GetBuffer();
    const uint64_t cSize = pBuffer ? pBuffer->GetSize() : 0;
    const uint64_t cPosition = aReader.GetBytePosition();
    const uint64_t cLeft = cSize > cPosition ? cSize - cPosition : 0;
    if (cLength > aMax || cLength > cLeft)
    {
        DecodeGuard::Fail();
        return {};
    }

    TiltedPhoques::String value;
    value.resize(static_cast<size_t>(cLength));
    // Also read for an empty string: the writer's byte write starts on a byte boundary even when it holds no bytes, and the reader has to follow.
    const bool cRead = aReader.ReadBytes(reinterpret_cast<uint8_t*>(value.data()), static_cast<size_t>(cLength));
    if (cLength != 0 && !cRead)
    {
        DecodeGuard::Fail();
        value.clear();
    }
    return value;
}
} // namespace BoundedString
