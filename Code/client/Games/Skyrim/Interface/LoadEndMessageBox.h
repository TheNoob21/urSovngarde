#pragma once

#include <cstdint>

// The message box show the game sends when it takes its loading screen down (SkyrimVR 1.4.15: the call at 0x140168502,
// returning to +0x168507). MessageBoxMenu then shows the last box in the game's pending list (SE 519819).
// A box queued while the loading screen is up is not shown by its own queueing, so it waits for this show:
// the "continue loading this save?" box is one.
namespace LoadEndMessageBox
{
inline constexpr uintptr_t kShowReturnOffset = 0x168507;

// Only that show, with no data, while the main menu or a loading screen is up, and with no box pending.
// A pending box is real: dropping its show leaves it waiting unseen, and whatever waits for its answer hangs.
constexpr bool ShouldDrop(bool aShow, bool aHasData, uintptr_t aCallerOffset, bool aLoadingOrMainMenu, uint32_t aPendingBoxes) noexcept
{
    return aShow && !aHasData && aCallerOffset == kShowReturnOffset && aLoadingOrMainMenu && aPendingBoxes == 0;
}

// The pending list's count; a list whose address is not known counts as one box pending, so a real box is never dropped.
constexpr uint32_t PendingOrUnknown(const uint32_t* apCount) noexcept
{
    return apCount ? *apCount : 1;
}
} // namespace LoadEndMessageBox
