#pragma once

/**
 * When the overlay (the browser page with the server list and chat) was dropped because it could not start or draw,
 * the player is told once on the HUD, so a flat player without the page knows to connect with the co-op power.
 * Kept apart from OverlayService so the rule can be tested without CEF.
 */
namespace OverlayNotice
{
// The HUD line, after the mod's name.
inline constexpr const char* kDroppedLine = "the overlay could not start, so there is no server list on screen; connect with the co-op power";

/**
 * @param aDropped The overlay was dropped after a failure (not merely absent by design, as on VR without a browser).
 * @param aShown The line was shown already this session.
 * @param aInGame The player is in the world (has 3D), so the HUD exists and the line is seen.
 * @return True when the line is to be shown now.
 */
[[nodiscard]] constexpr bool ShowDroppedLine(const bool aDropped, const bool aShown, const bool aInGame) noexcept
{
    return aDropped && !aShown && aInGame;
}
} // namespace OverlayNotice
