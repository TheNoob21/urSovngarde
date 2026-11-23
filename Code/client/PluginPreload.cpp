#include <TiltedOnlinePCH.h>

#include <base/system/Wine.h>

#include <CrashHandler.h>
#include <PluginPreloadGuard.h>

#include <algorithm>

#ifdef SKYRIMVR
namespace
{
constexpr std::wstring_view kPreloadSuffix = L"_preload.txt";

struct LoadCall
{
    const wchar_t* Path;
    HMODULE Module;
    DWORD Error;
};

void LoadPlugin(void* apCall)
{
    auto* pCall = static_cast<LoadCall*>(apCall);
    pCall->Module = LoadLibraryW(pCall->Path);
    pCall->Error = pCall->Module ? 0 : GetLastError();
}

void CallInitialize(void* apInitialize)
{
    reinterpret_cast<void(WINAPI*)()>(apInitialize)();
}

// The client's crash handler is already installed (TiltedOnlineApp::m_crashHandler). CrashGuard keeps it from reporting
// a fault caught here as the session's crash, which would use up its one report for a later, real one.
void Guarded(void (*apFunction)(void*), void* apArgument, plugin_preload::HardFault* apFault)
{
    struct Scope
    {
        Scope() noexcept { CrashGuard::Enter(); }
        ~Scope() { CrashGuard::Leave(); }
    } scope;
    plugin_preload::CallCatchingHardFaults(apFunction, apArgument, apFault);
}

// "EngineFixes_preload.txt" asks for "EngineFixes.dll" (in any case); empty for any other file name.
std::wstring PreloadedDllName(const std::wstring& acFileName)
{
    if (acFileName.size() <= kPreloadSuffix.size() || _wcsicmp(acFileName.c_str() + acFileName.size() - kPreloadSuffix.size(), kPreloadSuffix.data()) != 0)
        return {};
    return acFileName.substr(0, acFileName.size() - kPreloadSuffix.size()) + L".dll";
}
} // namespace

// Engine Fixes 7 has to be loaded, and its Initialize() called, before the game's static initialisers run, or it closes
// the game. On Windows an xSE plugin preloader (d3dx9_42.dll) does that from the game's first _initterm_e, for each
// Data\SKSE\Plugins\<name>_preload.txt. Under Wine there usually is none, so we do the same from our own _initterm_e
// hook (Games/Memory.cpp). apSlotValue is what the game's _initterm_e import held when we hooked it.
void PreloadPluginsAtCrtInit(const void* apSlotValue)
{
    if (!Base::IsRunningUnderWine())
        return;

    // Another preloader hooked the import before us and loads the plugins itself.
    const HMODULE cCrt = LoadLibraryW(L"api-ms-win-crt-runtime-l1-1-0.dll");
    if (!cCrt || apSlotValue != reinterpret_cast<const void*>(GetProcAddress(cCrt, "_initterm_e")))
        return;

    // The working directory is the game's.
    std::error_code ec;
    const std::filesystem::path cFolder = std::filesystem::current_path(ec) / L"Data\\SKSE\\Plugins";

    std::vector<std::wstring> dllNames;
    for (std::filesystem::directory_iterator it(cFolder, ec), end; !ec && it != end; it.increment(ec))
    {
        std::wstring dllName = PreloadedDllName(it->path().filename().wstring());
        if (!dllName.empty())
            dllNames.push_back(std::move(dllName));
    }

    // Windows lists a folder by name, Wine in the file system's order.
    std::sort(dllNames.begin(), dllNames.end(), [](const std::wstring& acLeft, const std::wstring& acRight) { return _wcsicmp(acLeft.c_str(), acRight.c_str()) < 0; });

    for (const std::wstring& cDllName : dllNames)
    {
        // Already loaded some other way: a plugin is never initialised twice.
        if (GetModuleHandleW(cDllName.c_str()))
            continue;

        const std::string cName(cDllName.begin(), cDllName.end());

        // Through the launcher's LdrLoadDll hook, so its Engine Fixes rules still apply (stubs/FileMapping.cpp).
        // A plugin that crashes in its load or its Initialize() is left out, and the game goes on, as with the Quest's preloader proxy.
        const std::wstring cPath = (cFolder / cDllName).wstring();
        spdlog::info("Plugin preload: loading {}", cName);
        spdlog::default_logger()->flush();
        LoadCall load{cPath.c_str(), nullptr, 0};
        plugin_preload::HardFault fault{};
        Guarded(&LoadPlugin, &load, &fault);
        if (fault.Code)
        {
            spdlog::error("Plugin preload: loading {} crashed (exception 0x{:08X} at {}); the game goes on without it", cName, fault.Code, fmt::ptr(fault.Address));
            continue;
        }
        if (!load.Module)
        {
            spdlog::warn("Plugin preload: {} did not load (error {})", cName, load.Error);
            continue;
        }

        spdlog::info("Plugin preload: {} loaded", cName);
        if (const auto pInitialize = reinterpret_cast<void(WINAPI*)()>(GetProcAddress(load.Module, "Initialize")))
        {
            spdlog::default_logger()->flush();
            Guarded(&CallInitialize, reinterpret_cast<void*>(pInitialize), &fault);
            if (fault.Code)
            {
                // The plugin may be half set up; its own log says how far it got.
                spdlog::error(
                    "Plugin preload: {} Initialize() crashed (exception 0x{:08X} at {}); the game goes on, the plugin may be half set up", cName, fault.Code,
                    fmt::ptr(fault.Address));
                spdlog::default_logger()->flush();
            }
        }
    }
}
#endif
