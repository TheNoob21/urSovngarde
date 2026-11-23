#include <catch2/catch.hpp>

#include <VRShout.h>

using VRShout::Decision;

TEST_CASE("Only a dragon shout or a howl shows as a VR shout", "[vr_shout]")
{
    REQUIRE(VRShout::IsShout(0xB));
    // A spell, a power, a lesser power (VR control menus are those) or a scroll is no shout.
    for (int32_t type : {0x0, 0x2, 0x3, 0x6, 0xD})
        REQUIRE_FALSE(VRShout::IsShout(type));
}

TEST_CASE("The voice actions are Skyrim.esm's", "[vr_shout]")
{
    REQUIRE(VRShout::IsVoiceAction(0x13008));
    REQUIRE(VRShout::IsVoiceAction(0x13457));
    REQUIRE(VRShout::IsVoiceAction(0x13458));
    REQUIRE(VRShout::IsVoiceAction(0x13459));
    // The attack actions are not.
    REQUIRE_FALSE(VRShout::IsVoiceAction(0x13004));
    REQUIRE_FALSE(VRShout::IsVoiceAction(0x13005));
    REQUIRE_FALSE(VRShout::IsVoiceAction(0));
}

TEST_CASE("A VR shout waits a moment for the game's own voice action, then is sent", "[vr_shout]")
{
    const uint64_t cCast = 10000;
    REQUIRE(VRShout::Decide(cCast, 0, cCast) == Decision::Wait);
    REQUIRE(VRShout::Decide(cCast, 0, cCast + VRShout::kOwnActionWait - 1) == Decision::Wait);
    REQUIRE(VRShout::Decide(cCast, 0, cCast + VRShout::kOwnActionWait) == Decision::Send);
    // Far later still sends: the frame was slow.
    REQUIRE(VRShout::Decide(cCast, 0, cCast + 5000) == Decision::Send);
}

TEST_CASE("A VR shout the game performed itself is not sent again", "[vr_shout]")
{
    const uint64_t cCast = 10000;
    // The game's voice action as the shout was held, or just after its cast.
    REQUIRE(VRShout::Decide(cCast, cCast - 1500, cCast) == Decision::Shown);
    REQUIRE(VRShout::Decide(cCast, cCast - VRShout::kOwnActionBefore, cCast) == Decision::Shown);
    REQUIRE(VRShout::Decide(cCast, cCast + 20, cCast + 60) == Decision::Shown);
    REQUIRE(VRShout::Decide(cCast, cCast + VRShout::kOwnActionAfter, cCast + 300) == Decision::Shown);
    // One from an earlier shout does not count.
    REQUIRE(VRShout::Decide(cCast, cCast - VRShout::kOwnActionBefore - 1, cCast + 60) == Decision::Send);
}

TEST_CASE("The same VR shout seen twice is shown once", "[vr_shout]")
{
    REQUIRE(VRShout::IsNewShout(10000, 0));
    REQUIRE_FALSE(VRShout::IsNewShout(10100, 10000));
    REQUIRE(VRShout::IsNewShout(10000 + VRShout::kSameShout, 10000));
    // The clock stepped back on a resync: a new shout.
    REQUIRE(VRShout::IsNewShout(5000, 10000));
}
