#pragma once

#include <cstddef>
#include <string>

//! Chat text is shown by the game's UI, which reads markup: tags are cut out before a line is relayed.
namespace ChatText
{
//! The longest chat or dialogue line that is relayed; the rest is cut off. A message may hold up to 64 KiB, and no real line is near that.
constexpr std::size_t kMaxLine = 1024;
//! The longest player name that is relayed.
constexpr std::size_t kMaxName = 128;

//! True for the characters a regular expression's whitespace class matches.
constexpr bool IsSpace(const char aChar) noexcept
{
    return aChar == ' ' || aChar == '\t' || aChar == '\n' || aChar == '\v' || aChar == '\f' || aChar == '\r';
}

/**
 * Cut the markup tags out of a line, in one pass over at most `aMaxLength` bytes.
 *
 * A tag is a '<', at least one character, then the first '>' after it. The white space after a tag is cut with it when another tag follows.
 * Text that is not part of a tag, an unclosed '<' included, is kept.
 *
 * @tparam T A string type with the std::string interface.
 * @param acText The line as received.
 * @param aMaxLength The most bytes of it that are read.
 * @return The line without tags.
 */
template <class T> T StripTags(const T& acText, const std::size_t aMaxLength) noexcept
{
    const std::size_t cEnd = acText.size() < aMaxLength ? acText.size() : aMaxLength;

    T result;
    result.reserve(cEnd);

    std::size_t i = 0;
    while (i < cEnd)
    {
        if (acText[i] != '<')
        {
            result.push_back(acText[i++]);
            continue;
        }

        std::size_t close = i + 1;
        while (close < cEnd && acText[close] != '>')
            ++close;

        // No closing '>' in what is left, or nothing between: this '<' is plain text.
        if (close >= cEnd || close == i + 1)
        {
            result.push_back(acText[i++]);
            continue;
        }

        std::size_t next = close + 1;
        std::size_t space = next;
        while (space < cEnd && IsSpace(acText[space]))
            ++space;
        // The white space goes with the tag only when another tag follows it.
        if (space > next && space < cEnd && acText[space] == '<')
            next = space;
        i = next;
    }
    return result;
}
} // namespace ChatText
