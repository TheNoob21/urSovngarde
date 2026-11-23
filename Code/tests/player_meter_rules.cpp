#include <catch2/catch.hpp>

#include <Services/PlayerMeterRules.h>

using namespace PlayerMeterRules;

TEST_CASE("The other player's bar goes up when you look at them or they are hurt", "[player_meter]")
{
    // Out of range: never.
    CHECK(Score(kRange + 1.f, 0.f, 0.1f, false) == 0.f);
    // At full health, only when looked at, and the hold angle is wider than the acquire angle.
    CHECK(Score(500.f, 10.f, 1.f, false) > 0.f);
    CHECK(Score(500.f, 25.f, 1.f, false) == 0.f);
    CHECK(Score(500.f, 25.f, 1.f, true) > 0.f);
    // Hurt: wherever you look.
    CHECK(Score(500.f, 170.f, 0.3f, false) > 0.f);
    // The view not read: only close by.
    CHECK(Score(1000.f, -1.f, 1.f, false) > 0.f);
    CHECK(Score(2000.f, -1.f, 1.f, false) == 0.f);
    // Looked at straight on beats looked at aside; hurt counts on top.
    CHECK(Score(500.f, 2.f, 1.f, false) > Score(500.f, 15.f, 1.f, false));
    CHECK(Score(500.f, 15.f, 0.2f, false) > Score(500.f, 15.f, 1.f, false));
}

TEST_CASE("A point on the flat screen is turned into degrees off its centre", "[player_meter]")
{
    CHECK(ScreenAngle(0.5f, 0.5f, 1.f) == 0.f);
    CHECK(ScreenAngle(1.f, 0.5f, 1.f) == Approx(kScreenWidthDegrees / 2.f));
    // Behind the camera, or not projected: never looked at.
    CHECK(ScreenAngle(0.5f, 0.5f, -1.f) == 180.f);
    CHECK(ScreenAngle(std::nanf(""), 0.5f, 1.f) == 180.f);
    // The centre of the screen is within the acquire angle, the edge is not.
    CHECK(Score(500.f, ScreenAngle(0.55f, 0.5f, 1.f), 1.f, false) > 0.f);
    CHECK(Score(500.f, ScreenAngle(0.95f, 0.5f, 1.f), 1.f, false) == 0.f);
}

TEST_CASE("After the game's SetTarget our player is put up, kept up, or waits for the game's own target", "[player_meter]")
{
    constexpr uint32_t cOurs = 0x100A;
    constexpr uint32_t cEnemy = 0x2002;

    // Ours sent, turned down by the game (not hurt): put up.
    CHECK(Decide(cOurs, cOurs, 0, 0) == AfterSetTarget::Force);
    // Ours up: told again only once the rearm time has passed.
    CHECK(Decide(cOurs, cOurs, cOurs, kRearmMs - 1) == AfterSetTarget::Leave);
    CHECK(Decide(cOurs, cOurs, cOurs, kRearmMs) == AfterSetTarget::Rearm);
    // The player's own target holds the meter: ours does not take it.
    CHECK(Decide(cOurs, cOurs, cEnemy, kRearmMs) == AfterSetTarget::Leave);
    // A hit on an enemy that the game took: remembered, so our player waits.
    CHECK(Decide(cEnemy, cOurs, cEnemy, 0) == AfterSetTarget::GameTarget);
    CHECK(Decide(cEnemy, 0, cEnemy, 0) == AfterSetTarget::GameTarget);
    // A target the game turned down is no target of its own.
    CHECK(Decide(cEnemy, cOurs, 0, 0) == AfterSetTarget::Leave);
    // Nobody wanted, or a clear: nothing.
    CHECK(Decide(cOurs, 0, 0, 0) == AfterSetTarget::Leave);
    CHECK(Decide(0, cOurs, 0, 0) == AfterSetTarget::Leave);
}
