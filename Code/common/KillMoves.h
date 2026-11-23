#pragma once

#include <cstdint>

// Killmoves are paired animations, which are not synced: while connected they are off unless the server allows them
// (Gameplay:bEnableKillMoves). The game's own switch is the global KillMove (Skyrim.esm, set to 1): the conditions of its
// killmove root idles read it, and its console turns killmoves off with "set KillMove to 0". It is the same form on SE and VR.
namespace KillMoves
{
constexpr uint32_t kGlobalId = 0x100F19;
constexpr float kOn = 1.f;
constexpr float kOff = 0.f;

// The global's value for this connection: off while connected to a server that does not allow killmoves, the game's own
// value (on) otherwise, as before the connection.
constexpr float Wanted(bool aConnected, bool aServerAllows) noexcept
{
    return aConnected && !aServerAllows ? kOff : kOn;
}
} // namespace KillMoves
