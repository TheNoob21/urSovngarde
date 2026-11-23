#pragma once

// A crash dump small enough to send with a crash report.
// It holds every thread's stack and registers, the module list, and whatever memory the caller's callback adds.
// The crash handler adds the objects the crashing registers point at.
// It never holds the game's data segments, which make the full dump about 1 GB.
//
// Header-only so that the tests can write one of their own process.

#include <Windows.h>
#include <DbgHelp.h>

#include <algorithm>

namespace SmallDump
{
// Crash reports only carry small dumps under 4 MB; above this a dump is written again with fewer stacks.
constexpr uint64_t cTarget = 3500ull * 1024;

namespace detail
{
// Wraps the caller's callback. The second, smaller pass keeps every thread and its registers but only the crashing thread's stack.
// Clearing a thread's ThreadWriteStack flag does not keep its stack out; removing the stack's memory range does.
// So each other thread's stack range is noted as dbghelp reports the thread, and handed back when it asks for memory to remove.
struct Pass
{
    PMINIDUMP_CALLBACK_INFORMATION Inner;
    DWORD CrashingThread;
    bool OneStack;
    struct Range
    {
        ULONG64 Base;
        ULONG Size;
    } Stacks[512]{};
    size_t StackCount = 0;
    size_t NextRemoved = 0;
};

inline BOOL CALLBACK Callback(PVOID apParam, const PMINIDUMP_CALLBACK_INPUT apInput, PMINIDUMP_CALLBACK_OUTPUT apOutput) noexcept
{
    auto* pass = static_cast<Pass*>(apParam);
    if (pass->OneStack && apInput && apOutput)
    {
        if (apInput->CallbackType == ThreadCallback && apInput->Thread.ThreadId != pass->CrashingThread &&
            pass->StackCount < std::size(pass->Stacks))
        {
            const ULONG64 low = (std::min)(apInput->Thread.StackBase, apInput->Thread.StackEnd);
            const ULONG64 high = (std::max)(apInput->Thread.StackBase, apInput->Thread.StackEnd);
            if (high > low && high - low < 0x10000000)
                pass->Stacks[pass->StackCount++] = {low, static_cast<ULONG>(high - low)};
            return TRUE;
        }
        if (apInput->CallbackType == RemoveMemoryCallback)
        {
            if (pass->NextRemoved >= pass->StackCount)
                return FALSE; // nothing more to remove
            apOutput->MemoryBase = pass->Stacks[pass->NextRemoved].Base;
            apOutput->MemorySize = pass->Stacks[pass->NextRemoved].Size;
            ++pass->NextRemoved;
            return TRUE;
        }
    }
    if (pass->Inner && pass->Inner->CallbackRoutine)
        return pass->Inner->CallbackRoutine(pass->Inner->CallbackParam, apInput, apOutput);
    return TRUE;
}

inline uint64_t WriteOnce(HANDLE ahFile, PEXCEPTION_POINTERS apInfo, Pass& aPass, DWORD& aError, const DWORD aExtraType) noexcept
{
    aError = 0;
    if (!SetFilePointerEx(ahFile, {}, nullptr, FILE_BEGIN) || !SetEndOfFile(ahFile))
    {
        aError = GetLastError();
        return 0;
    }

    MINIDUMP_EXCEPTION_INFORMATION exception{};
    exception.ThreadId = GetCurrentThreadId();
    exception.ExceptionPointers = apInfo;
    exception.ClientPointers = FALSE;

    MINIDUMP_CALLBACK_INFORMATION callback{};
    callback.CallbackRoutine = &Callback;
    callback.CallbackParam = &aPass;

    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | aExtraType);
    const BOOL written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), ahFile, type, apInfo ? &exception : nullptr, nullptr, &callback);
    if (!written)
        aError = GetLastError();

    LARGE_INTEGER size{};
    GetFileSizeEx(ahFile, &size);
    if (!written)
        return 0;
    return static_cast<uint64_t>(size.QuadPart);
}
} // namespace detail

// Writes to the caller's exclusively created file and returns its size in bytes, or 0 with `aError` set.
// When the whole dump comes out above `aTarget`, it is written again with only the crashing thread's stack.
// `apCallback` adds memory (its MemoryCallback); `apRestart` runs before each pass, so that it can hand out its memory again.
// `aExtraType` adds MINIDUMP_TYPE flags: the freeze dump asks for the handle table, which names what a waiting thread waits on.
inline uint64_t Write(
    HANDLE ahFile, PEXCEPTION_POINTERS apInfo, PMINIDUMP_CALLBACK_INFORMATION apCallback, DWORD& aError, void (*apRestart)() noexcept = nullptr, uint64_t aTarget = cTarget,
    const DWORD aExtraType = 0) noexcept
{
    detail::Pass pass{apCallback, GetCurrentThreadId(), false};
    if (apRestart)
        apRestart();
    const uint64_t size = detail::WriteOnce(ahFile, apInfo, pass, aError, aExtraType);
    if (size == 0 || size <= aTarget)
        return size;
    pass.OneStack = true;
    if (apRestart)
        apRestart();
    return detail::WriteOnce(ahFile, apInfo, pass, aError, aExtraType);
}

} // namespace SmallDump
