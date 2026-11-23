#pragma once

#include <string_view>

// The CSV and the compiled address overrides contain RVAs for this exact executable.
namespace VRSupportedVersion
{
inline constexpr std::string_view kVersion = "1.4.15.0";

constexpr bool IsSupported(std::string_view aVersion) noexcept
{
    return aVersion == kVersion;
}
} // namespace VRSupportedVersion
