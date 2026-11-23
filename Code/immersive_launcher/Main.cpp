
#include "Utils/Error.h"
#include "launcher.h"
#include "SystemPreload.h"
#include "oobe/PackageSelection.h"
#include <Windows.h>
#include <combaseapi.h>
#include <cstdio>
#include <cstring>
#include <base/system/Wine.h>
#include <base/threading/ThreadUtils.h>

#include "utils/ComUtils.h"
#include "script_extender/SEMemoryBlock.h"

#include <TiltedCore/Platform.hpp>

extern void CoreStubsInit();

extern "C"
{
    __declspec(dllexport) int NvOptimusEnablement = 1;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

auto kSystemPreloadDlls = {
    L"\\dinput8.dll",   // < Skyrim early init hook
    L"\\dsound.dll",    // < breaks DSound init in game code
    L"\\nvspcap64.dll", // < Nvidia overlay, needs to be loaded before d3d11 device creation
                        // < X360CE v3 is buggy with COM hooks
    L"\\xinput9_1_0.dll", L"\\xinput1_1.dll", L"\\xinput1_2.dll", L"\\xinput1_3.dll", L"\\xinput1_4.dll", L"\\version.dll"};

namespace
{
// What PreloadSystemDlls chose for dinput8.dll (SystemPreload.h). No log is open yet then, so Launcher.cpp writes it to
// tp_client.log later, next to the loader's notes.
system_preload::Dinput8Choice s_dinput8Choice{true, system_preload::Dinput8Reason::kNotWine};
} // namespace

const char* SystemDinput8PreloadNote() noexcept
{
    return system_preload::Describe(s_dinput8Choice);
}

static void PreloadSystemDlls()
{
    auto loadSystemDll = [](auto dll)
    {
        wchar_t systemPath[512];
        GetSystemDirectoryW(systemPath, _countof(systemPath));

        wcscat_s(systemPath, dll);

        LoadLibraryW(systemPath);
    };

    // Windows preloads dinput8.dll as always; Wine leaves it out (SystemPreload.h), unless URSOVNGARDE_PRELOAD_SYSTEM_DINPUT8 says otherwise.
    char setting[16] = {};
    const DWORD cSettingLength = GetEnvironmentVariableA("URSOVNGARDE_PRELOAD_SYSTEM_DINPUT8", setting, sizeof(setting));
    const bool cSettingPresent = cSettingLength > 0 && cSettingLength < sizeof(setting);
    s_dinput8Choice = system_preload::ChooseDinput8Preload(Base::IsRunningUnderWine(), cSettingPresent ? setting : nullptr);

    // Wine's log shows this (+debugstr), DebugView does on Windows.
    char line[256];
    std::snprintf(line, sizeof(line), "%s\n", SystemDinput8PreloadNote());
    OutputDebugStringA(line);

    for (auto dll : kSystemPreloadDlls)
    {
        if (!s_dinput8Choice.Preload && wcscmp(dll, L"\\dinput8.dll") == 0)
            continue;
        loadSystemDll(dll);
    }
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--packages") == 0)
        return oobe::SelectPackage();

    Base::SetCurrentThreadName("MainLauncherThread");

    // memory block for Script Extender reserved as early as we can
    script_extender::SEMemoryBlock b;
    if (!b.Good())
    {
        Die(L"Failed to pre-reserve script extender zone.\nAsk Force!");
        return -1;
    }

#ifdef SKYRIMVR
    // Address space near the game for SKSE plugin trampolines (see memory/NearImageReserve.cpp).
    extern bool NearImageReserveInit();
    if (!NearImageReserveInit())
        OutputDebugStringA("NearImageReserve: could not reserve a pool below the image\n");
#endif

    PreloadSystemDlls();
    CoreStubsInit();

    ComScope cs;
    TP_UNUSED(cs);

    auto ret = launcher::StartUp(argc, argv);

    return ret;
}
