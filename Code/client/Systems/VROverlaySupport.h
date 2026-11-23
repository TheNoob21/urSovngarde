#pragma once

//! Which in-game overlays this run can use, decided once per process.
//!
//! Standalone headsets (Quest under GameNative) run the game through OpenComposite, whose IVROverlay
//! dashboard and raw-image calls are stubs that abort the process (STUBBED() -> OOVR_ABORT).
//! The in-game panel draws through the game's eye images instead. Without CEF it stays absent,
//! while connect.txt and the Papyrus actions remain available.
namespace VROverlaySupport
{
//! True when libcef.dll can be loaded and URSOVNGARDE_NO_CEF is not set.
//! libcef.dll is delay-loaded by the launcher, so nothing calls into it unless this returned true.
bool CefAvailable() noexcept;

//! True when the game's openvr_api.dll is OpenComposite (OpenXR translation) rather than SteamVR's.
bool IsOpenComposite() noexcept;

//! True when OpenVR overlays (the dashboard, banner and meter) are safe to create:
//! not OpenComposite, and URSOVNGARDE_NO_VR_OVERLAY is not set.
bool VROverlaysAvailable() noexcept;
} // namespace VROverlaySupport
