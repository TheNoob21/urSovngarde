#include <catch2/catch.hpp>

#include <SneakState.h>

using namespace SneakState;

TEST_CASE("A copy sneaks after its owner's sneak toggle, though the owner's state came from before it", "[sneak_state]")
{
    constexpr uint32_t cStanding = 0x00000082; // running and moving forward, not sneaking
    constexpr uint32_t cSneaking = cStanding | kBit;

    // The owner stood when it toggled: it sneaks now.
    CHECK(Of(AfterAction(kActionSneak, cStanding, cStanding)));
    // The owner sneaked when it toggled: it stands now.
    CHECK_FALSE(Of(AfterAction(kActionSneak, cSneaking, cSneaking)));
    // Whatever this game's copy had: the owner's toggle decides.
    CHECK(Of(AfterAction(kActionSneak, cStanding, cSneaking)));
    CHECK_FALSE(Of(AfterAction(kActionSneak, cSneaking, cStanding)));

    // Only the sneak bit changes.
    CHECK((AfterAction(kActionSneak, cStanding, cStanding) & ~kBit) == cStanding);
}

TEST_CASE("Any other action leaves the copy's sneak state as the owner's flags put it", "[sneak_state]")
{
    constexpr uint32_t cSneaking = 0x00000082 | kBit;
    constexpr uint32_t cOtherAction = 0x13005; // any action but the sneak toggle
    CHECK(AfterAction(cOtherAction, 0, cSneaking) == cSneaking);
    CHECK(AfterAction(cOtherAction, cSneaking, 0) == 0);
    CHECK(AfterAction(0, cSneaking, cSneaking) == cSneaking);
}

TEST_CASE("The sneak bit is ActorState1's, beside the swimming bit the client already reads", "[sneak_state]")
{
    CHECK(kBit == 0x200);
    CHECK((kBit << 1) == 0x400); // swimming, bit 10 (ActorState::IsSwimming)
}
