#include <catch2/catch.hpp>

#include <ChatText.h>

#include <chrono>
#include <string>

TEST_CASE("Tags are cut out of a chat line", "[chat_text]")
{
    using ChatText::StripTags;
    constexpr std::size_t cMax = ChatText::kMaxLine;

    SECTION("plain text is kept")
    {
        REQUIRE(StripTags(std::string("hello there"), cMax) == "hello there");
        REQUIRE(StripTags(std::string(), cMax).empty());
    }

    SECTION("a tag goes, its text stays")
    {
        REQUIRE(StripTags(std::string("a <b>bold</b> word"), cMax) == "a bold word");
        REQUIRE(StripTags(std::string("<font color='#ff0000'>red"), cMax) == "red");
    }

    SECTION("white space between two tags goes with them, white space before text stays")
    {
        REQUIRE(StripTags(std::string("<b>  <i>x"), cMax) == "x");
        REQUIRE(StripTags(std::string("<b>  x"), cMax) == "  x");
        REQUIRE(StripTags(std::string("<b> \t\n <i> <u>y"), cMax) == "y");
    }

    SECTION("an unclosed or empty tag is plain text")
    {
        REQUIRE(StripTags(std::string("1 < 2"), cMax) == "1 < 2");
        REQUIRE(StripTags(std::string("a<>b"), cMax) == "a<>b");
        REQUIRE(StripTags(std::string("<<a>x"), cMax) == "x");
    }

    SECTION("a line is cut at the limit")
    {
        REQUIRE(StripTags(std::string("abcdef"), 3) == "abc");
        // A tag the cut falls inside is not closed, so it stays as text.
        REQUIRE(StripTags(std::string("ab<cd>"), 5) == "ab<cd");
    }

    SECTION("a long run of opening brackets does not take long")
    {
        const std::string cHostile(65535, '<');
        const auto cStart = std::chrono::steady_clock::now();
        const auto cOut = StripTags(cHostile, cMax);
        const auto cSpent = std::chrono::steady_clock::now() - cStart;
        REQUIRE(cOut.size() == cMax);
        REQUIRE(cSpent < std::chrono::seconds(2));
    }
}
