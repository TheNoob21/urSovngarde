#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

// The encoding structs name TiltedCore's types without the namespace, as Message.h does for the message tests.
using namespace TiltedPhoques;

#include <PartyWeather.h>

TEST_CASE("A party keeps the weather reported in each worldspace", "[party_weather]")
{
    const GameId cTamriel(0, 0x3C);
    const GameId cSolstheim(2, 0x800);
    const GameId cClear(0, 0x81A);
    const GameId cAshStorm(2, 0x1234);

    PartyWeather weathers;
    CHECK(weathers.Get(cTamriel) == GameId{});

    weathers.Set(cTamriel, cClear);
    weathers.Set(cSolstheim, cAshStorm);
    // A member coming into range in Skyrim is not given Solstheim's ash storm, reported last.
    CHECK(weathers.Get(cTamriel) == cClear);
    CHECK(weathers.Get(cSolstheim) == cAshStorm);

    // Indoors there is no weather to keep.
    weathers.Set(GameId{}, cAshStorm);
    CHECK(weathers.Get(GameId{}) == GameId{});
}

TEST_CASE("A member indoors or where nobody reported a weather is given none", "[party_weather]")
{
    const GameId cTamriel(0, 0x3C);
    const GameId cSolstheim(2, 0x800);

    PartyWeather weathers;
    weathers.Set(cTamriel, GameId(0, 0x81A));
    // WeatherService sends nothing for an empty id, which the client would take for a weather it cannot find.
    CHECK(static_cast<bool>(weathers.Get(cTamriel)));
    CHECK_FALSE(static_cast<bool>(weathers.Get(cSolstheim)));
    CHECK_FALSE(static_cast<bool>(weathers.Get(GameId{})));
}
