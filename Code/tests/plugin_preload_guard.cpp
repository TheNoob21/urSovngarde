// The fault guard around the launcher's own SKSE plugin preload (client/PluginPreloadGuard.h): only the hard faults the
// Quest's preloader proxy catches are caught; everything else goes on to the handlers above.

#ifdef _WIN32
#include <catch2/catch.hpp>

#include <client/PluginPreloadGuard.h>

namespace
{
using plugin_preload::CallCatchingHardFaults;
using plugin_preload::CatchHardFault;
using plugin_preload::HardFault;

constexpr DWORD kHardFaults[] = {static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION), static_cast<DWORD>(EXCEPTION_ILLEGAL_INSTRUCTION),
                                 static_cast<DWORD>(EXCEPTION_PRIV_INSTRUCTION), static_cast<DWORD>(EXCEPTION_STACK_OVERFLOW),
                                 static_cast<DWORD>(EXCEPTION_IN_PAGE_ERROR),    static_cast<DWORD>(EXCEPTION_INT_DIVIDE_BY_ZERO)};

// A C++ exception, a breakpoint, MSVC's thread-name exception, an arbitrary software code, and two close neighbours of
// the hard faults (a float divide by zero and an array bounds check).
constexpr DWORD kOtherExceptions[] = {0xE06D7363ul, static_cast<DWORD>(EXCEPTION_BREAKPOINT),         0x406D1388ul,
                                      0xE0000001ul, static_cast<DWORD>(EXCEPTION_FLT_DIVIDE_BY_ZERO), static_cast<DWORD>(EXCEPTION_ARRAY_BOUNDS_EXCEEDED)};

int s_calls = 0;

void Returns(void* apArgument)
{
    ++s_calls;
    *static_cast<int*>(apArgument) = 42;
}

void Raises(void* apCode)
{
    ++s_calls;
    RaiseException(*static_cast<DWORD*>(apCode), 0, 0, nullptr);
}

// Whether a guarded call let this exception go on to the next handler up: true when the outer __except saw it.
bool PassesThrough(DWORD aCode, HardFault* apFault)
{
    __try
    {
        CallCatchingHardFaults(&Raises, &aCode, apFault);
    }
    __except (GetExceptionCode() == aCode ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        return true;
    }
    return false;
}
} // namespace

TEST_CASE("The fault guard catches exactly the Quest proxy's hard faults", "[wine.preload]")
{
    int where = 0;
    for (const DWORD cCode : kHardFaults)
    {
        EXCEPTION_RECORD record{};
        record.ExceptionCode = cCode;
        record.ExceptionAddress = &where;
        EXCEPTION_POINTERS pointers{&record, nullptr};
        HardFault fault{};
        CHECK(CatchHardFault(&pointers, &fault) == EXCEPTION_EXECUTE_HANDLER);
        CHECK(fault.Code == cCode);
        CHECK(fault.Address == &where);
    }
    for (const DWORD cCode : kOtherExceptions)
    {
        EXCEPTION_RECORD record{};
        record.ExceptionCode = cCode;
        record.ExceptionAddress = &where;
        EXCEPTION_POINTERS pointers{&record, nullptr};
        HardFault fault{};
        CHECK(CatchHardFault(&pointers, &fault) == EXCEPTION_CONTINUE_SEARCH);
        CHECK(fault.Code == 0);
        CHECK(fault.Address == nullptr);
    }
}

TEST_CASE("A plugin that loads and initialises cleanly leaves no fault behind", "[wine.preload]")
{
    s_calls = 0;
    int result = 0;
    HardFault fault{0xDEAD, &result};
    CallCatchingHardFaults(&Returns, &result, &fault);
    CHECK(s_calls == 1);
    CHECK(result == 42);
    CHECK(fault.Code == 0);
    CHECK(fault.Address == nullptr);
}

// Catch2's own vectored handler reports an access violation, illegal instruction, stack overflow or integer divide by
// zero as a failed test before any __except sees it, so those four are covered by the filter test above only.
// The other two hard faults are really raised here, through the same __try as the launcher's.
TEST_CASE("A hard fault raised in a guarded call is caught and described", "[wine.preload]")
{
    for (const DWORD cCode : {static_cast<DWORD>(EXCEPTION_PRIV_INSTRUCTION), static_cast<DWORD>(EXCEPTION_IN_PAGE_ERROR)})
    {
        s_calls = 0;
        HardFault fault{};
        CHECK_FALSE(PassesThrough(cCode, &fault));
        CHECK(s_calls == 1);
        CHECK(fault.Code == cCode);
        CHECK(fault.Address != nullptr);
    }
}

TEST_CASE("Anything but a hard fault goes on to the handlers above the preload", "[wine.preload]")
{
    for (const DWORD cCode : kOtherExceptions)
    {
        HardFault fault{};
        CHECK(PassesThrough(cCode, &fault));
        CHECK(fault.Code == 0);
    }
}
#endif
