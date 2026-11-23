#pragma once

// The Quest's dinput8 preloader proxy catches only hard faults from a plugin's load and its Initialize(), and so does
// the launcher's own preload (PluginPreload.cpp). Everything else (C++ exceptions, breakpoints, the thread-name
// exception) takes its normal course. Under Wine a __try in our code is found through the merged exception table.
// Windows-only and free of the game, for the tests (Code/tests/plugin_preload_guard.cpp).

#include <malloc.h>
#include <windows.h>

namespace plugin_preload
{
struct HardFault
{
    unsigned long Code;  // 0 when nothing was caught
    const void* Address; // where it happened
};

inline int CatchHardFault(const EXCEPTION_POINTERS* apInfo, HardFault* apFault) noexcept
{
    switch (apInfo->ExceptionRecord->ExceptionCode)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        apFault->Code = apInfo->ExceptionRecord->ExceptionCode;
        apFault->Address = apInfo->ExceptionRecord->ExceptionAddress;
        return EXCEPTION_EXECUTE_HANDLER;
    default: return EXCEPTION_CONTINUE_SEARCH;
    }
}

// Calls apFunction(apArgument). A hard fault inside it is caught and described in *apFault; *apFault is zeroed first.
// After a caught stack overflow the guard page is set again.
// It holds nothing with a destructor, because __try cannot share a function with one (C2712).
inline void CallCatchingHardFaults(void (*apFunction)(void*), void* apArgument, HardFault* apFault)
{
    *apFault = HardFault{};
    __try
    {
        apFunction(apArgument);
    }
    __except (CatchHardFault(GetExceptionInformation(), apFault))
    {
        // A stack overflow used the thread's guard page; without this the next one ends the process with no report.
        if (apFault->Code == EXCEPTION_STACK_OVERFLOW)
            _resetstkoflw();
    }
}
} // namespace plugin_preload
