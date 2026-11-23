#include <cstdint>

#include <catch2/catch.hpp>

#include <Games/Magic/MagicSystem.h>

TEST_CASE("concentration spells, wards and invisibility go as casts; the rest go as projectiles", "[castsync]")
{
    using namespace MagicSystem;

    // Flames: a concentration spell.
    CHECK(IsSentAsCast(CONCENTRATION, false, false));
    // Firebolt: fire and forget, a projectile.
    CHECK_FALSE(IsSentAsCast(FIRE_AND_FORGET, false, false));
    // Lesser Ward and Invisibility, whatever their casting type.
    CHECK(IsSentAsCast(CONCENTRATION, true, false));
    CHECK(IsSentAsCast(FIRE_AND_FORGET, false, true));
    // A constant effect is never cast.
    CHECK_FALSE(IsSentAsCast(CONSTANT_EFFECT, false, false));
}

TEST_CASE("another player's copy hits an NPC with an attack spell, never with a heal or a buff", "[castsync]")
{
    using namespace MagicSystem;

    CHECK(CopyEffectIsHit(false, false));
    CHECK_FALSE(CopyEffectIsHit(true, false));
    CHECK_FALSE(CopyEffectIsHit(false, true));
    CHECK_FALSE(CopyEffectIsHit(true, true));
}

TEST_CASE("Slow Time and beast form shouts are left to their effect sync, so they apply once", "[castsync]")
{
    using namespace MagicSystem;

    // Unrelenting Force: a voice cast that goes as a cast.
    CHECK(IsVoiceCast(VOICE_POWER));
    CHECK(IsVoiceCastSent(false));
    // Slow Time, Beast Form and Vampire Lord: their effect is synced, so the cast is not.
    CHECK_FALSE(IsVoiceCastSent(true));
}
