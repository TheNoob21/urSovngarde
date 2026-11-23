#include <catch2/catch.hpp>

#include <FlyState.h>
#include <VRAddressOverrides.h>

TEST_CASE("a flyer's flight state is read from bits 18-20", "[flystate]")
{
    REQUIRE(FlyState::Of(0) == FlyState::kNone);
    REQUIRE(FlyState::Of(FlyState::kCruising << FlyState::kShift) == FlyState::kCruising);
    REQUIRE(FlyState::Of(0xFFFFFFFFu) == 7);
    REQUIRE(FlyState::Of((FlyState::kHovering << FlyState::kShift) | 0x3FFFFu | 0xE000000u) == FlyState::kHovering);
}

TEST_CASE("in the air is anything but the ground or a perch", "[flystate]")
{
    REQUIRE_FALSE(FlyState::InTheAir(FlyState::kNone));
    REQUIRE_FALSE(FlyState::InTheAir(FlyState::kPerching));
    REQUIRE(FlyState::InTheAir(FlyState::kTakeOff));
    REQUIRE(FlyState::InTheAir(FlyState::kCruising));
    REQUIRE(FlyState::InTheAir(FlyState::kHovering));
    REQUIRE(FlyState::InTheAir(FlyState::kLanding));
    REQUIRE(FlyState::InTheAir(FlyState::kAction));
}

TEST_CASE("a copy takes its owner's state but keeps its own flight state and kept bits", "[flystate]")
{
    constexpr uint32_t cKnock = 0xE000000;
    const uint32_t cOwner = (FlyState::kCruising << FlyState::kShift) | 0x1234u | (2u << 25);
    const uint32_t cOwn = (FlyState::kNone << FlyState::kShift) | 0x0F00u | (5u << 25);
    const uint32_t cCopy = FlyState::CopyFlags1(cOwner, cOwn, cKnock);
    REQUIRE(FlyState::Of(cCopy) == FlyState::kNone);
    REQUIRE((cCopy & cKnock) == (cOwn & cKnock));
    REQUIRE((cCopy & 0xFFFFu) == 0x1234u);
}

TEST_CASE("VR sets a flight state through the game's own setter", "[flystate]")
{
    // AE 37947, SE 36922: VR 0x1406110F0, the function the graph's flight handlers call.
    size_t count = 0;
    uint32_t offset = 0;
    for (const auto& cEntry : kVRAddressOverrides)
    {
        if (cEntry.id != 37947)
            continue;
        ++count;
        offset = cEntry.offset;
    }
    CHECK(count == 1);
    CHECK(offset == 0x6110f0);
}

TEST_CASE("A flyer's copy is set to its owner's flight state through the game", "[flystate]")
{
    constexpr uint32_t cHovering = FlyState::kHovering << FlyState::kShift;
    constexpr uint32_t cCruising = FlyState::kCruising << FlyState::kShift;
    // A dragon or a vampire lord with a movement controller, whose owner's state differs.
    CHECK(FlyState::SetThroughGame(true, true, cHovering, 0));
    CHECK(FlyState::SetThroughGame(true, true, 0, cCruising));
    CHECK(FlyState::SetThroughGame(true, true, cHovering | 0x1234u, cCruising | 0x1234u));
    // The same state, other bits apart, no flyer, or no controller: nothing.
    CHECK_FALSE(FlyState::SetThroughGame(true, true, cHovering | 0x1u, cHovering));
    CHECK_FALSE(FlyState::SetThroughGame(false, true, cHovering, 0));
    CHECK_FALSE(FlyState::SetThroughGame(true, false, cHovering, 0));
}
