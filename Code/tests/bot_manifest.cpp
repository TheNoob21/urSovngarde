#include <catch2/catch.hpp>
#include <bot/BotManifest.h>
#include <sstream>

TEST_CASE("Bot manifest parses plugin names, sizes and load slots", "[bot]")
{
    std::istringstream input("# name, size, slot, light\nSkyrim.esm\t100 0 0\nCity Overhaul.esl\t400 3 1\n");
    std::vector<BotManifest::Entry> entries;
    REQUIRE(BotManifest::Read(input, entries));
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].Name == "Skyrim.esm");
    CHECK(entries[0].Size == 100);
    CHECK(entries[1].Name == "City Overhaul.esl");
    CHECK(entries[1].Id == 3);
    CHECK(entries[1].IsLite);
}

TEST_CASE("Bot manifest rejects malformed and incomplete rows", "[bot]")
{
    for (const char* text : {"", "Skyrim.esm\t0 0 0", "Skyrim.esm\t-1 0 0", "Skyrim.esm\t100 65536 0", "Skyrim.esm\t100 0 2", "Skyrim.esm\t100 0 0 extra"})
    {
        std::istringstream input(text);
        std::vector<BotManifest::Entry> entries;
        CHECK_FALSE(BotManifest::Read(input, entries));
    }
}
