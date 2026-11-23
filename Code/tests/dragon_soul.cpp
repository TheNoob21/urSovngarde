#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <catch2/catch.hpp>

#include <DragonSoul.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/NotifyDragonSoul.h>
#include <Structs/ServerSettings.h>

using TiltedPhoques::Buffer;
using namespace std::chrono_literals;

TEST_CASE("a game keeps dragons out of its own absorb only when the server gives the souls", "[dragonsoul]")
{
    REQUIRE(DragonSoul::KeepsDragonsOut(true, true, true));
    // The server giving none, or no connection: the game's own absorb runs.
    REQUIRE_FALSE(DragonSoul::KeepsDragonsOut(false, true, true));
    REQUIRE_FALSE(DragonSoul::KeepsDragonsOut(true, false, true));
    REQUIRE_FALSE(DragonSoul::KeepsDragonsOut(false, false, true));
    // A game without the co-op menu plugin cannot take a soul the server gives: the game's own absorb runs, so it is never left with none.
    REQUIRE_FALSE(DragonSoul::KeepsDragonsOut(true, true, false));
}

TEST_CASE("a reconnect does not use the previous server's dragon soul setting", "[dragonsoul]")
{
    // Connection 1: its server gives souls, and the game keeps dragons out of its own absorb.
    ServerSettings settings{};
    settings.DragonSouls = true;
    REQUIRE(DragonSoul::KeepsDragonsOut(settings.DragonSouls, true, true));

    // The disconnect forgets the settings (TransportService::OnDisconnected stores a default ServerSettings).
    settings = ServerSettings{};
    REQUIRE_FALSE(settings.DragonSouls);

    // Connection 2 before its server has answered: not accepted yet, so nothing is kept out, even with a stale true.
    REQUIRE_FALSE(DragonSoul::KeepsDragonsOut(settings.DragonSouls, false, true));
    REQUIRE_FALSE(DragonSoul::KeepsDragonsOut(true, false, true));

    // The new server answers that it gives no souls: still nothing is kept out.
    settings.DragonSouls = false;
    REQUIRE_FALSE(DragonSoul::KeepsDragonsOut(settings.DragonSouls, true, true));
}

TEST_CASE("the server's settings say whether it gives dragon souls", "[dragonsoul]")
{
    for (const bool cOn : {false, true})
    {
        ServerSettings settings{};
        settings.DragonSouls = cOn;

        Buffer buffer(64);
        Buffer::Writer writer(&buffer);
        settings.Serialize(writer);
        Buffer::Reader reader(&buffer);
        ServerSettings back{};
        back.Deserialize(reader);
        REQUIRE(back.DragonSouls == cOn);
        REQUIRE(back == settings);
    }
}

TEST_CASE("a dragon's soul is given out once, at its first death", "[dragonsoul]")
{
    REQUIRE(DragonSoul::ShouldGrant(true, true, false));
    // Given already: a death reported again after a hand-off, or by a copy made again.
    REQUIRE_FALSE(DragonSoul::ShouldGrant(true, true, true));
    // Not a dragon, or not dead.
    REQUIRE_FALSE(DragonSoul::ShouldGrant(true, false, false));
    REQUIRE_FALSE(DragonSoul::ShouldGrant(false, true, false));
}

TEST_CASE("each game takes the soul once, absorbed when loaded here, alone when not", "[dragonsoul]")
{
    using DragonSoul::Outcome;
    REQUIRE(DragonSoul::OnGrant(false, false, false, true) == Outcome::kWaitNear);
    REQUIRE(DragonSoul::OnGrant(false, false, false, false) == Outcome::kSoulOnly);
    // A second word for the same dragon gives nothing more.
    REQUIRE(DragonSoul::OnGrant(true, false, false, true) == Outcome::kIgnore);
    REQUIRE(DragonSoul::OnGrant(true, false, false, false) == Outcome::kIgnore);
    // The Dragonborn's lands keep the game's own story; a dragon the game never absorbs gives nothing.
    REQUIRE(DragonSoul::OnGrant(false, true, false, true) == Outcome::kLeftToGame);
    REQUIRE(DragonSoul::OnGrant(false, false, true, true) == Outcome::kNoSoul);
}

TEST_CASE("a dragon is never saved out of the game's own absorb, but a body whose soul was had stays out of it", "[dragonsoul]")
{
    // A living dragon co-op kept out: back in for the save and after a disconnect (a main-quest dragon absorbable alone).
    REQUIRE(DragonSoul::BackInGameAbsorb(true, false, false));
    REQUIRE(DragonSoul::BackInGameAbsorb(true, false, true));
    // A body whose soul never came here: the game's own absorb may still give it.
    REQUIRE(DragonSoul::BackInGameAbsorb(true, true, false));
    // A body whose soul this player had: kept out, or alone it would be absorbed again.
    REQUIRE_FALSE(DragonSoul::BackInGameAbsorb(true, true, true));
    // One co-op never put out of it (a dragon its own record keeps out, say) is left as it is.
    REQUIRE_FALSE(DragonSoul::BackInGameAbsorb(false, false, false));
}

TEST_CASE("a dragon loaded here waits for this player as the game's own absorb does, then gives the soul alone", "[dragonsoul]")
{
    using DragonSoul::Wait;
    const std::chrono::steady_clock::time_point cSince{100s};
    REQUIRE(DragonSoul::WhileWaiting(true, DragonSoul::kAbsorbRange, cSince, cSince) == Wait::kAbsorb);
    REQUIRE(DragonSoul::WhileWaiting(true, DragonSoul::kAbsorbRange + 1.f, cSince, cSince + 1s) == Wait::kKeepWaiting);
    REQUIRE(DragonSoul::WhileWaiting(true, DragonSoul::kAbsorbRange + 1.f, cSince, cSince + DragonSoul::kAbsorbWait) == Wait::kSoulOnly);
    // Gone from here before this player came: the soul alone.
    REQUIRE(DragonSoul::WhileWaiting(false, 0.f, cSince, cSince) == Wait::kSoulOnly);
}

TEST_CASE("the dragon soul message carries the dragon", "[dragonsoul][encoding.packets]")
{
    Buffer buff(1000);
    NotifyDragonSoul notify;
    notify.ServerId = 0x12345;

    Buffer::Writer writer(&buff);
    notify.Serialize(writer);
    Buffer::Reader reader(&buff);

    const ServerMessageFactory factory;
    auto pMessage = factory.Extract(reader);
    REQUIRE(pMessage);
    REQUIRE(pMessage->GetOpcode() == kNotifyDragonSoul);
    auto pNotify = TiltedPhoques::CastUnique<NotifyDragonSoul>(std::move(pMessage));
    REQUIRE(*pNotify == notify);
}
