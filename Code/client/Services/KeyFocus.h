#pragma once

#include <cstdint>

//! Whether this game reads its debug keys (F6 connect, F3, F7, F8) this frame. GetAsyncKeyState reads the keyboard of the
//! whole desktop, so with two games on one PC one press reached both: the game whose window is in front takes it.
//! On SE the renderer's own window is asked; VR has none hooked (Hook_Renderer_Init is off there), so the foreground
//! window's process is compared with this one. One exception: when no foreground window is found (process 0, as under Wine),
//! the keys are still read, so this is not fail-closed isolation.
namespace KeyFocus
{
//! aHasMainWindow / aMainWindowInFront: the renderer's window (SE). aForegroundProcess: the process of the window in front,
//! 0 when there is none (then the keys are read); aOwnProcess: this game's. Only another process verifiably in front keeps the keys from this game.
inline bool KeysReachGame(const bool aHasMainWindow, const bool aMainWindowInFront, const uint32_t aForegroundProcess, const uint32_t aOwnProcess) noexcept
{
    if (aHasMainWindow)
        return aMainWindowInFront;
    // No window in front (Wine, a window changing): nothing shows another game has the keyboard, so they are read.
    if (aForegroundProcess == 0)
        return true;
    return aForegroundProcess == aOwnProcess;
}
} // namespace KeyFocus
