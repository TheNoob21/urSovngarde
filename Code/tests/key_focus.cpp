#include <catch2/catch.hpp>

#include <Services/KeyFocus.h>

TEST_CASE("Only the game in front reads the debug keys", "[key_focus]")
{
    using KeyFocus::KeysReachGame;

    // SE: the renderer's window decides.
    CHECK(KeysReachGame(true, true, 0, 7));
    CHECK_FALSE(KeysReachGame(true, false, 7, 7));

    // VR: the window in front must be this process's.
    CHECK(KeysReachGame(false, false, 7, 7));
    CHECK_FALSE(KeysReachGame(false, false, 9, 7));
    // No window in front (Wine, a window changing): read, as nothing shows another game has the keyboard.
    CHECK(KeysReachGame(false, false, 0, 7));
}
