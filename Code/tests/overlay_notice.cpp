#include <catch2/catch.hpp>

#include <Services/OverlayNotice.h>

using namespace OverlayNotice;

TEST_CASE("The dropped-overlay line shows once, only after a failure and only in the world", "[overlay]")
{
    // Waits for the world: at start-up there is no HUD to show it on.
    CHECK_FALSE(ShowDroppedLine(true, false, false));
    CHECK(ShowDroppedLine(true, false, true));
    // Once.
    CHECK_FALSE(ShowDroppedLine(true, true, true));
    // Never when nothing failed.
    CHECK_FALSE(ShowDroppedLine(false, false, true));
    CHECK_FALSE(ShowDroppedLine(false, true, true));
}
