#include <TiltedOnlinePCH.h>

#include <Systems/VROverlaySupport.h>
#ifdef SKYRIMVR
#include <Games/Skyrim/VR/VRRuntime.h>
#endif

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace VROverlaySupport
{
namespace
{
// A nonempty value not starting with '0' is a force-off override; unset, empty or a value starting with '0' leaves it on.
// Read from the environment only.
bool EnvSet(const char* acpName) noexcept
{
    const char* pValue = std::getenv(acpName);
    return pValue && *pValue && pValue[0] != '0';
}

} // namespace

bool CefAvailable() noexcept
{
    static std::atomic<int> s_result{-1};
    if (const int cResult = s_result.load(); cResult >= 0)
        return cResult == 1;

    static const bool s_forcedOff = EnvSet("URSOVNGARDE_NO_CEF");
    if (s_forcedOff)
    {
        spdlog::debug("VROverlaySupport: CEF overlay off (URSOVNGARDE_NO_CEF)");
        s_result = 0;
        return false;
    }
    bool decided = true;
#ifdef SKYRIMVR
    // The runtime is only known once OpenVR is up; before that the answer is not cached.
    decided = GetModuleHandleW(L"openvr_api.dll") != nullptr;
    // The in-game panel draws into the game's eye images, including on OpenComposite.
#endif
    if (GetModuleHandleW(L"libcef.dll") || LoadLibraryW(L"libcef.dll"))
    {
        if (decided)
            s_result = 1;
        return true;
    }
    spdlog::debug("VROverlaySupport: libcef.dll could not be loaded ({}); running without the CEF overlay", GetLastError());
    s_result = 0;
    return false;
}

bool IsOpenComposite() noexcept
{
#ifdef SKYRIMVR
    return VRRuntime::IsOpenComposite();
#else
    return false;
#endif
}

bool VROverlaysAvailable() noexcept
{
    static const bool s_forcedOff = EnvSet("URSOVNGARDE_NO_VR_OVERLAY");
    if (s_forcedOff)
        return false;
    return !IsOpenComposite();
}
} // namespace VROverlaySupport
