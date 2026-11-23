#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <istream>
#include <iterator>
#include <map>
#include <string>

// Address Library format 5: 96-byte header followed by ID-indexed u32 RVAs.
namespace VersionDbV5
{
using AddressMap = std::map<unsigned long long, unsigned long long>;

inline bool Load(std::istream& aFile, const std::array<int, 4>& acExpectedVersion, std::string& aModule, AddressMap& aOffsets, AddressMap& aReverse)
{
    auto read = [&aFile](auto& aValue)
    {
        return static_cast<bool>(aFile.read(reinterpret_cast<char*>(&aValue), sizeof(aValue)));
    };
    std::uint32_t format = 0, version[4]{}, pointerSize = 0, dataFormat = 0, count = 0;
    char name[64]{};
    if (!read(format) || format != 5)
        return false;
    for (auto& part : version)
        if (!read(part))
            return false;
    if (!aFile.read(name, sizeof(name)) || !read(pointerSize) || !read(dataFormat) || !read(count))
        return false;
    for (std::size_t i = 0; i < 4; ++i)
        if (acExpectedVersion[i] < 0 || version[i] != static_cast<std::uint32_t>(acExpectedVersion[i]))
            return false;
    const auto nameEnd = std::find(std::begin(name), std::end(name), '\0');
    if (nameEnd == std::begin(name) || nameEnd == std::end(name) || pointerSize != 8 || dataFormat != 0 || count == 0 || count > 16 * 1024 * 1024)
        return false;

    const auto body = aFile.tellg();
    if (body == std::istream::pos_type(-1))
        return false;
    aFile.seekg(0, std::ios::end);
    const auto end = aFile.tellg();
    if (end == std::istream::pos_type(-1) || end < body || static_cast<std::uint64_t>(end - body) < static_cast<std::uint64_t>(count) * sizeof(std::uint32_t))
        return false;
    aFile.seekg(body);
    if (!aFile)
        return false;

    AddressMap parsed, parsedReverse;
    for (std::uint32_t id = 0; id < count; ++id)
    {
        std::uint32_t offset = 0;
        if (!read(offset))
            return false;
        if (offset != 0)
        {
            parsed[id] = offset;
            parsedReverse[offset] = id;
        }
    }
    aModule.assign(name, nameEnd);
    aOffsets.swap(parsed);
    aReverse.swap(parsedReverse);
    return true;
}
} // namespace VersionDbV5
