#pragma once

namespace DInputSetting
{
namespace detail
{
constexpr char LowerAscii(char aChar) noexcept
{
    return aChar >= 'A' && aChar <= 'Z' ? static_cast<char>(aChar - 'A' + 'a') : aChar;
}

constexpr bool IsWord(const char* apText, const char* apWord) noexcept
{
    for (; *apText && *apWord; ++apText, ++apWord)
    {
        if (LowerAscii(*apText) != *apWord)
            return false;
    }
    return *apText == *apWord;
}
} // namespace detail

constexpr bool IsOff(const char* apSetting) noexcept
{
    using detail::IsWord;
    return IsWord(apSetting, "0") || IsWord(apSetting, "off") || IsWord(apSetting, "false") || IsWord(apSetting, "no");
}
} // namespace DInputSetting
