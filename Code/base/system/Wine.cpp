#include "Wine.h"

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace Base
{

bool IsRunningUnderWine() noexcept
{
#if defined(_WIN32)
    const HMODULE cNtdll = GetModuleHandleW(L"ntdll.dll");
    return cNtdll && GetProcAddress(cNtdll, "wine_get_version") != nullptr;
#else
    return false;
#endif
}

} // namespace Base
