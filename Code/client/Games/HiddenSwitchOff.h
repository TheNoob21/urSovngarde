#pragma once

// A switch-off of a reference this game hides for another player's copy (kHiddenHere), made by someone else:
// a quest's script, or the game. Its disabled flag is set already, so the switch changes nothing that can be seen, and
// the reference comes back on when it is released, and is saved as on (KNOWN_ISSUES, Followers).
// Only logged for now (`HiddenSwitchOff:`), to learn from a two-player session whether it catches script switch-offs only;
// this game's own late-landing disable must not be among them before anything acts on it.
namespace HiddenSwitchOff
{
/**
 * @param aHiddenHere The reference is hidden here for another player's copy.
 * @param aWasDisabled It was already switched off when the switch-off came.
 * @param aOwnSwitch The switch is this game's own model reload (ScopedReferenceEnableOverride).
 */
constexpr bool IsForeign(bool aHiddenHere, bool aWasDisabled, bool aOwnSwitch) noexcept
{
    return aHiddenHere && aWasDisabled && !aOwnSwitch;
}
} // namespace HiddenSwitchOff
