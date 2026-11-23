#include <catch2/catch.hpp>

#include <immersive_launcher/oobe/TitlePath.h>

TEST_CASE("the game's folder keeps the backslash of a drive root", "[launcher]")
{
    REQUIRE(oobe::TitlePathOf(LR"(A:\SkyrimSE.exe)") == LR"(A:\)");
    REQUIRE(oobe::TitlePathOf(LR"(C:\Games\Skyrim Special Edition\SkyrimSE.exe)") == LR"(C:\Games\Skyrim Special Edition)");
}
