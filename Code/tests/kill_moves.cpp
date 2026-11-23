#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <KillMoves.h>
#include <Structs/ServerSettings.h>

using TiltedPhoques::Buffer;

TEST_CASE("killmoves are off while connected unless the server allows them", "[killmoves]")
{
    REQUIRE(KillMoves::Wanted(true, false) == KillMoves::kOff);
    REQUIRE(KillMoves::Wanted(true, true) == KillMoves::kOn);
    // Not connected: the game's own value, whatever the last server said.
    REQUIRE(KillMoves::Wanted(false, false) == KillMoves::kOn);
    REQUIRE(KillMoves::Wanted(false, true) == KillMoves::kOn);
}

TEST_CASE("a game without server settings has killmoves off while connected", "[killmoves]")
{
    // The default settings (a disconnect stores them) do not allow killmoves.
    const ServerSettings settings{};
    REQUIRE_FALSE(settings.KillMoves);
    REQUIRE(KillMoves::Wanted(true, settings.KillMoves) == KillMoves::kOff);
}

TEST_CASE("the game's KillMove global is the one in Skyrim.esm", "[killmoves]")
{
    REQUIRE(KillMoves::kGlobalId == 0x100F19u);
}

TEST_CASE("the server settings carry independent killmove and projectile camera permissions", "[killmoves]")
{
    REQUIRE_FALSE(ServerSettings{}.ProjectileKillCam);
    for (const bool cOn : {false, true})
    {
        ServerSettings settings{};
        settings.KillMoves = cOn;
        settings.ProjectileKillCam = !cOn;

        Buffer buffer(64);
        Buffer::Writer writer(&buffer);
        settings.Serialize(writer);
        Buffer::Reader reader(&buffer);
        ServerSettings back{};
        back.Deserialize(reader);
        REQUIRE(back.KillMoves == cOn);
        REQUIRE(back.ProjectileKillCam == !cOn);
        REQUIRE(back == settings);
    }
}
