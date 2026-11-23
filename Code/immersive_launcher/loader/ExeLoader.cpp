/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

// Changes:
// - 2021/2/24: Moved TLS routine.
// - 2021/2/25: Implemented CEG decryption method.

#define SPDLOG_WCHAR_FILENAMES
#include <TiltedCore/Filesystem.hpp>
#include <spdlog/formatter.h>
#include <cstdio>
#include <cstring>
#include <winternl.h>
#include <cstddef>
#include <base/system/Wine.h>

#include "ExeLoader.h"
#include "SehTableMerge.h"
#include "steam/SteamCeg.h"
#include "utils/Error.h"
#include "utils/NtInternal.h"

// TlsMemory.cpp
size_t TlsPlaceholderOffset(const void* apTlsBase) noexcept;
size_t TlsPlaceholderSize() noexcept;

#if defined(_M_AMD64)
typedef enum _FUNCTION_TABLE_TYPE
{
    RF_SORTED,
    RF_UNSORTED,
    RF_CALLBACK
} FUNCTION_TABLE_TYPE;

typedef struct _DYNAMIC_FUNCTION_TABLE
{
    LIST_ENTRY Links;
    PRUNTIME_FUNCTION FunctionTable;
    LARGE_INTEGER TimeStamp;

    ULONG_PTR MinimumAddress;
    ULONG_PTR MaximumAddress;
    ULONG_PTR BaseAddress;

    PGET_RUNTIME_FUNCTION_CALLBACK Callback;
    PVOID Context;
    PWSTR OutOfProcessCallbackDll;
    FUNCTION_TABLE_TYPE Type;
    ULONG EntryCount;
} DYNAMIC_FUNCTION_TABLE, *PDYNAMIC_FUNCTION_TABLE;
#endif

#if defined(_M_AMD64)
namespace
{
bool s_threadNameHandlerInstalled = false;
// What the Wine exception-table merge did, for the client's log (empty off Wine).
char s_exceptionTableNote[160] = {};

// Thread naming: 0x406D1388 is raised to hand a debugger a thread's name, and the code that raises it catches it itself.
// Under Wine that catch can be in our code, which has unwind data only through the merged exception table, so the exception
// would go uncaught and end the process. This handler stays on next to the merge as a second line, as it ran on
// the Quest. URSOVNGARDE_THREADNAME_VEH=0 (off, false, no) turns it off. Wine has applied the name
// before any vectored handler runs (dispatch_exception, dlls/ntdll/exception.c), so resuming loses nothing.
LONG CALLBACK ResumeAfterThreadName(PEXCEPTION_POINTERS apInfo) noexcept
{
    const EXCEPTION_RECORD* pRecord = apInfo->ExceptionRecord;
    if (pRecord->ExceptionCode == 0x406D1388 && (pRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE) == 0 && !IsDebuggerPresent())
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

// On under Wine unless URSOVNGARDE_THREADNAME_VEH says off; a value that is not off turns it on anywhere.
bool ThreadNameHandlerWanted() noexcept
{
    char setting[16] = {};
    const DWORD cSettingLength = GetEnvironmentVariableA("URSOVNGARDE_THREADNAME_VEH", setting, sizeof(setting));
    const bool cSettingPresent = cSettingLength > 0 && cSettingLength < sizeof(setting);
    const bool cSettingOff =
        cSettingPresent && (std::strcmp(setting, "0") == 0 || _stricmp(setting, "off") == 0 || _stricmp(setting, "false") == 0 || _stricmp(setting, "no") == 0);
    return cSettingPresent ? !cSettingOff : Base::IsRunningUnderWine();
}
} // namespace
#endif

// Launcher.cpp writes this to the client's log once it is open: the Wine exception-table merge's entry counts, or that
// it was skipped. nullptr off Wine.
const char* ExceptionTableMergeNote() noexcept
{
#if defined(_M_AMD64)
    return s_exceptionTableNote[0] ? s_exceptionTableNote : nullptr;
#else
    return nullptr;
#endif
}

// Launcher.cpp writes this to the client's log once it is open.
bool ThreadNameHandlerInstalled() noexcept
{
#if defined(_M_AMD64)
    return s_threadNameHandlerInstalled;
#else
    return false;
#endif
}

// TODO: move me to wstring util..
std::wstring ConvertStringToWstring(const std::string_view str)
{
    int nChars = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, str.data(), static_cast<int>(str.length()), NULL, 0);

    std::wstring wstrTo;
    if (nChars)
    {
        wstrTo.resize(nChars);
        if (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, str.data(), static_cast<int>(str.length()), &wstrTo[0], nChars))
        {
            return wstrTo;
        }
    }
    return {};
}

ExeLoader::ExeLoader(uint32_t aLoadLimit, TFuncHandler aFuncHandler)
    : m_loadLimit(aLoadLimit)
    , m_pFuncHandler(aFuncHandler)
{
}

void ExeLoader::LoadImports(const IMAGE_NT_HEADERS* apNtHeader)
{
    const auto* importDirectory = &apNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const auto* descriptor = GetTargetRVA<const IMAGE_IMPORT_DESCRIPTOR>(importDirectory->VirtualAddress);

    while (descriptor->Name)
    {
        auto dllName = ConvertStringToWstring(GetTargetRVA<char>(descriptor->Name));

        HMODULE hMod = LoadLibraryW(dllName.c_str());
        if (!hMod)
        {
            auto msg = fmt::format(L"Failed to find dll: {}", dllName);
            Die(msg.c_str(), true);
            continue;
        }

        // "don't load"
        if (*reinterpret_cast<uint32_t*>(hMod) == 0xFFFFFFFF)
        {
            descriptor++;
            continue;
        }

        auto nameTableEntry = GetTargetRVA<uintptr_t>(descriptor->OriginalFirstThunk);
        auto addressTableEntry = GetTargetRVA<uintptr_t>(descriptor->FirstThunk);

        if (!descriptor->OriginalFirstThunk)
        {
            nameTableEntry = GetTargetRVA<uintptr_t>(descriptor->FirstThunk);
        }

        while (*nameTableEntry)
        {
            FARPROC function;
            const char* functionName{nullptr};

            // is this an ordinal-only import?
            if (IMAGE_SNAP_BY_ORDINAL(*nameTableEntry))
            {
                function = GetProcAddress(hMod, MAKEINTRESOURCEA(IMAGE_ORDINAL(*nameTableEntry)));
            }
            else
            {
                auto import = GetTargetRVA<IMAGE_IMPORT_BY_NAME>(static_cast<uint32_t>(*nameTableEntry));

                function = m_pFuncHandler(hMod, import->Name);
                functionName = import->Name;
            }

            if (!function)
            {
                wchar_t pathName[1024]{};
                GetModuleFileNameW(hMod, pathName, sizeof(pathName) - 1);

                auto thunkName = ConvertStringToWstring(functionName ? functionName : "<unknown>");
                auto msg = fmt::format(L"Failed to find thunk {} in dll {}", thunkName, pathName);
                Die(msg.c_str(), true);
            }

            *addressTableEntry = (uintptr_t)function;

            nameTableEntry++;
            addressTableEntry++;
        }

        descriptor++;
    }
}

void ExeLoader::LoadSections(const IMAGE_NT_HEADERS* apNtHeader)
{
    auto* section = IMAGE_FIRST_SECTION(apNtHeader);
    for (int i = 0; i < apNtHeader->FileHeader.NumberOfSections; i++)
    {
        uint8_t* targetAddress = GetTargetRVA<uint8_t>(section->VirtualAddress);
        const void* sourceAddress = m_pBinary + section->PointerToRawData;

        if (targetAddress >= (reinterpret_cast<uint8_t*>(m_moduleHandle) + m_loadLimit))
        {
            return;
        }

        if (section->SizeOfRawData > 0)
        {
            uint32_t sizeOfData = std::min(section->SizeOfRawData, section->Misc.VirtualSize);

            memcpy(targetAddress, sourceAddress, sizeOfData);

            DWORD oldProtect;
            VirtualProtect(targetAddress, sizeOfData, PAGE_EXECUTE_READWRITE, &oldProtect);
        }

        section++;
    }
}

void ExeLoader::LoadTLS(const IMAGE_NT_HEADERS* apNtHeader, const IMAGE_NT_HEADERS* apSourceNt)
{
    if (apNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size)
    {
        const auto* sourceTls = GetTargetRVA<IMAGE_TLS_DIRECTORY>(apNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress);
        const auto* targetTls = GetTargetRVA<IMAGE_TLS_DIRECTORY>(apSourceNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress);

        *(DWORD*)(sourceTls->AddressOfIndex) = 0;

        LPVOID tlsBase = *(LPVOID*)__readgsqword(0x58);

        // Only the start of the TLS section itself (_tls_start) may come before the placeholder. Anything else of ours there,
        // such as a logger's thread id, would be written over the game's per-thread data, and the game's over it.
        constexpr size_t cSectionStart = 0x20;
        const size_t cGameTlsSize = sourceTls->EndAddressOfRawData - sourceTls->StartAddressOfRawData + sourceTls->SizeOfZeroFill;
        const size_t cPlaceholderOffset = TlsPlaceholderOffset(tlsBase);
        if (cPlaceholderOffset > cSectionStart || cGameTlsSize > cPlaceholderOffset + TlsPlaceholderSize())
        {
            wchar_t msg[256];
            swprintf_s(
                msg, L"The launcher's TLS placeholder is at +0x%zX (at most +0x%zX allowed) and holds 0x%zX bytes; the game needs 0x%zX.", cPlaceholderOffset, cSectionStart,
                TlsPlaceholderSize(), cGameTlsSize);
            Die(msg, true);
        }

        DWORD oldProtect;
        VirtualProtect(reinterpret_cast<LPVOID>(targetTls->StartAddressOfRawData), sourceTls->EndAddressOfRawData - sourceTls->StartAddressOfRawData, PAGE_READWRITE, &oldProtect);

        std::memcpy(tlsBase, reinterpret_cast<void*>(sourceTls->StartAddressOfRawData), sourceTls->EndAddressOfRawData - sourceTls->StartAddressOfRawData);
        std::memcpy((void*)targetTls->StartAddressOfRawData, reinterpret_cast<void*>(sourceTls->StartAddressOfRawData), sourceTls->EndAddressOfRawData - sourceTls->StartAddressOfRawData);
    }
}

void ExeLoader::LoadExceptionTable(IMAGE_NT_HEADERS* apNtHeader)
{
    IMAGE_DATA_DIRECTORY* exceptionDirectory = &apNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];

    RUNTIME_FUNCTION* functionList = GetTargetRVA<RUNTIME_FUNCTION>(exceptionDirectory->VirtualAddress);
    DWORD entryCount = exceptionDirectory->Size / sizeof(RUNTIME_FUNCTION);

    // has no use - inverted function tables get used instead from Ldr; we have no influence on those
    if (!RtlAddFunctionTable(functionList, entryCount, (DWORD64)GetModuleHandle(nullptr)))
    {
        Die(L"Setting exception handlers failed.", false);
    }

    // replace the function table stored for debugger purposes (though we just added it above)
    {
        PLIST_ENTRY(NTAPI * rtlGetFunctionTableListHead)(VOID);
        rtlGetFunctionTableListHead = (decltype(rtlGetFunctionTableListHead))GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetFunctionTableListHead");

        if (rtlGetFunctionTableListHead)
        {
            auto tableListHead = rtlGetFunctionTableListHead();
            auto tableListEntry = tableListHead->Flink;

            while (tableListEntry != tableListHead)
            {
                auto functionTable = CONTAINING_RECORD(tableListEntry, DYNAMIC_FUNCTION_TABLE, Links);

                if (functionTable->BaseAddress == reinterpret_cast<ULONG_PTR>(m_moduleHandle))
                {
                    if (functionTable->FunctionTable != functionList)
                    {
                        DWORD oldProtect;
                        VirtualProtect(functionTable, sizeof(DYNAMIC_FUNCTION_TABLE), PAGE_READWRITE, &oldProtect);

                        functionTable->EntryCount = entryCount;
                        functionTable->FunctionTable = functionList;

                        VirtualProtect(functionTable, sizeof(DYNAMIC_FUNCTION_TABLE), oldProtect, &oldProtect);
                    }
                }

                tableListEntry = functionTable->Links.Flink;
            }
        }
    }
}

#if defined(_M_AMD64)
static_assert(sizeof(RUNTIME_FUNCTION) == sizeof(seh_table::RuntimeFunction));
static_assert(offsetof(RUNTIME_FUNCTION, BeginAddress) == offsetof(seh_table::RuntimeFunction, BeginAddress));
static_assert(offsetof(RUNTIME_FUNCTION, EndAddress) == offsetof(seh_table::RuntimeFunction, EndAddress));
static_assert(offsetof(RUNTIME_FUNCTION, UnwindData) == offsetof(seh_table::RuntimeFunction, UnwindData));

// Writes one exception table for the game and the launcher into free memory of the image (SehTableMerge.h).
// Changes nothing when it returns false.
bool ExeLoader::BuildMergedExceptionTable(
    const IMAGE_NT_HEADERS* apNtHeader, const IMAGE_DATA_DIRECTORY& acLauncherTable, uint32_t aLauncherImageSize, IMAGE_DATA_DIRECTORY& aMerged)
{
    using seh_table::RuntimeFunction;

    const IMAGE_DATA_DIRECTORY& gameExceptions = apNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!gameExceptions.VirtualAddress || !acLauncherTable.VirtualAddress)
        return false;

    // Each table must lie inside its own image before anything reads it.
    if (uint64_t{gameExceptions.VirtualAddress} + gameExceptions.Size > apNtHeader->OptionalHeader.SizeOfImage ||
        uint64_t{acLauncherTable.VirtualAddress} + acLauncherTable.Size > aLauncherImageSize)
        return false;

    const auto* pGame = GetTargetRVA<const RuntimeFunction>(gameExceptions.VirtualAddress);
    const size_t cGameCount = gameExceptions.Size / sizeof(RuntimeFunction);
    const auto* pLauncher = GetTargetRVA<const RuntimeFunction>(acLauncherTable.VirtualAddress);
    const size_t cLauncherCount = acLauncherTable.Size / sizeof(RuntimeFunction);

    // The load buffer (MemoryLayout.cpp) goes on past the end of the game's image up to m_loadLimit, unused.
    const uint32_t cFreeBegin = (apNtHeader->OptionalHeader.SizeOfImage + 0xFFF) & ~0xFFFu;

    seh_table::Plan plan;
    if (seh_table::PlanMerge(pGame, cGameCount, gameExceptions.VirtualAddress, pLauncher, cLauncherCount, acLauncherTable.VirtualAddress, cFreeBegin, m_loadLimit, plan) !=
        seh_table::Result::kOk)
        return false;

    seh_table::WriteMerged(pGame, cGameCount, pLauncher, cLauncherCount, GetTargetRVA<RuntimeFunction>(plan.TableRva));

    aMerged.VirtualAddress = plan.TableRva;
    aMerged.Size = plan.SizeBytes;
    std::snprintf(
        s_exceptionTableNote, sizeof(s_exceptionTableNote), "urSovngarde ExeLoader: merged exception table for Wine, %zu game + %zu launcher entries", cGameCount, cLauncherCount);
    return true;
}
#endif

uint32_t ExeLoader::Rva2Offset(uint32_t aRva) noexcept
{
    const auto* dos = GetRVA<const IMAGE_DOS_HEADER>(0);
    const auto* nt = GetRVA<const IMAGE_NT_HEADERS>(dos->e_lfanew);

    auto* section = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        if (aRva >= section[i].VirtualAddress && (aRva < section[i].VirtualAddress + section[i].Misc.VirtualSize))
        {
            return static_cast<uint32_t>(aRva - (section[i].VirtualAddress - section[i].PointerToRawData));
        }
    }

    return 0;
}

void ExeLoader::DecryptCeg(IMAGE_NT_HEADERS* apSourceNt)
{
    auto entry = apSourceNt->OptionalHeader.AddressOfEntryPoint;
    // analyze executable sections if the entry point is already protected
    if (*GetOffset<uint32_t>(entry) != 0x000000e8)
        return;

    const auto* section = IMAGE_FIRST_SECTION(apSourceNt);
    for (int i = 0; i < apSourceNt->FileHeader.NumberOfSections; i++)
    {
        if (!_strcmpi(reinterpret_cast<const char*>(section[i].Name), ".text"))
        {
            break;
        }
    }

    steam::CEGLocationInfo info{GetOffset<uint8_t>(entry), {GetOffset<uint8_t>(section->VirtualAddress), section->SizeOfRawData}};

    auto realEntry = steam::CrackCEGInPlace(info);

    apSourceNt->FileHeader.NumberOfSections--;
    apSourceNt->OptionalHeader.AddressOfEntryPoint = static_cast<uint32_t>(realEntry);
}

// Skyrim VR 1.4.15 can deadlock on its logo under emulation. TESInitThread clears its busy flag and then joins itself,
// taking the BSThread object's lock to close its handle; the main thread, seeing the flag, runs ~BSThread, whose join takes
// the same lock and waits for the worker to exit. Whichever gets the lock first decides, and emulator timing lets the main
// thread win almost every time (Wine: "RtlpWaitForCriticalSection ... blocked by" the main thread).
// The worker's self-join call is replaced with a 5-byte NOP; the main thread's join still closes the handle.
// Applied under Wine only (GameNative and other emulators), or when URSOVNGARDE_LOGO_PATCH is 1; 0 turns it off.
// On a Quest 3, patched launches passed the logo and unpatched ones froze there.
void ExeLoader::PatchStartupJoinRace(const IMAGE_NT_HEADERS* apNtHeader)
{
    constexpr uint32_t cSkyrimVR1415Timestamp = 0x5AEADAA0;
    constexpr uint32_t cSelfJoinCallRva = 0x5C0613; // VA 0x1405C0613
    constexpr uint8_t cExpected[5] = {0xE8, 0x48, 0x27, 0x68, 0x00};
    constexpr uint8_t cNop5[5] = {0x0F, 0x1F, 0x44, 0x00, 0x00};

    if (apNtHeader->FileHeader.TimeDateStamp != cSkyrimVR1415Timestamp)
        return;

    char setting[16] = {};
    const DWORD cSettingLength = GetEnvironmentVariableA("URSOVNGARDE_LOGO_PATCH", setting, sizeof(setting));
    const bool cUnderWine = Base::IsRunningUnderWine();
    // Explicit false values override Wine detection; an absent or oversized setting uses that default.
    const bool cSettingPresent = cSettingLength > 0 && cSettingLength < sizeof(setting);
    const bool cSettingOff =
        cSettingPresent && (std::strcmp(setting, "0") == 0 || _stricmp(setting, "off") == 0 || _stricmp(setting, "false") == 0 || _stricmp(setting, "no") == 0);
    const bool cWanted = cSettingPresent ? !cSettingOff : cUnderWine;
    if (!cWanted)
        return;

    if (cSelfJoinCallRva + sizeof(cExpected) > apNtHeader->OptionalHeader.SizeOfImage)
        return;
    auto* pCall = GetTargetRVA<uint8_t>(cSelfJoinCallRva);
    if (std::memcmp(pCall, cNop5, sizeof(cNop5)) == 0)
    {
        OutputDebugStringA("urSovngarde ExeLoader: startup join patch already applied\n");
        return;
    }
    if (std::memcmp(pCall, cExpected, sizeof(cExpected)) != 0)
    {
        // The log file isn't open yet this early; Wine's debugstr channel (and DebugView on Windows) shows this.
        OutputDebugStringA("urSovngarde ExeLoader: startup join patch skipped, unexpected bytes at RVA 0x5C0613\n");
        return;
    }

    DWORD oldProtect;
    if (!VirtualProtect(pCall, sizeof(cNop5), PAGE_EXECUTE_READWRITE, &oldProtect))
        return;
    std::memcpy(pCall, cNop5, sizeof(cNop5));
    VirtualProtect(pCall, sizeof(cNop5), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), pCall, sizeof(cNop5));
    OutputDebugStringA("urSovngarde ExeLoader: TESInitThread self-join replaced with a NOP (startup deadlock under emulation)\n");
}

bool ExeLoader::Load(const uint8_t* apProgramBuffer)
{
    m_pBinary = apProgramBuffer;
    m_moduleHandle = GetModuleHandleW(nullptr);

    // validate the target
    const auto* dosHeader = GetRVA<const IMAGE_DOS_HEADER>(0);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return false;
    }

    // remove protections
    auto* ntHeader = GetRVA<IMAGE_NT_HEADERS>(dosHeader->e_lfanew);
    DecryptCeg(ntHeader);

    // LoadSections would silently leave the game partially mapped.
    if (ntHeader->OptionalHeader.SizeOfImage > m_loadLimit)
        return false;

    // these point to launcher.exe's headers
    auto* sourceHeader = GetTargetRVA<IMAGE_DOS_HEADER>(0);
    auto* sourceNtHeader = GetTargetRVA<IMAGE_NT_HEADERS>(sourceHeader->e_lfanew);

    // store EP
    m_pEntryPoint = GetTargetRVA<void>(ntHeader->OptionalHeader.AddressOfEntryPoint);

    // store these as they will get overridden by the target's header
    // but we really need them in order to not break debugging for cosi.
    auto sourceChecksum = sourceNtHeader->OptionalHeader.CheckSum;
    auto sourceTimestamp = sourceNtHeader->FileHeader.TimeDateStamp;
    auto sourceDebugDir = sourceNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    // Other plugins find this mod's calls through it, and the game has no exports of its own.
    auto sourceExportDir = sourceNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
#if defined(_M_AMD64)
    // The launcher's own functions, for the merged exception table below.
    const auto sourceExceptionDir = sourceNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    const uint32_t cSourceSizeOfImage = sourceNtHeader->OptionalHeader.SizeOfImage;
#endif

    LoadSections(ntHeader);
    PatchStartupJoinRace(ntHeader);

    // skse64_plugin_preloader (proxy d3dx9_42_dll and others?) may hook
    // _initterm_e during LoadImports(), so we have to ensure that IAT entry exists.
    // The simplest way to make sure all SkyrimSE IAT entries exist when mods expect 
    // them to is to switch to those headers earlier than we used to
    DWORD oldProtect;
    VirtualProtect(sourceNtHeader, 0x1000, PAGE_EXECUTE_READWRITE, &oldProtect);
    sourceNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = ntHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    LoadImports(ntHeader);
#if defined(_M_AMD64)
    LoadExceptionTable(ntHeader);
    LoadTLS(ntHeader, sourceNtHeader);

    // Wine finds unwind data through the headers copied below, so it would only know the game's functions and an
    // exception raised in our own code would end the process. Windows keeps the table it loaded and is left as it was.
    IMAGE_DATA_DIRECTORY mergedExceptionDir{};
    if (Base::IsRunningUnderWine())
    {
        if (BuildMergedExceptionTable(ntHeader, sourceExceptionDir, cSourceSizeOfImage, mergedExceptionDir))
            ntHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION] = mergedExceptionDir;
        else
            std::snprintf(
                s_exceptionTableNote, sizeof(s_exceptionTableNote), "urSovngarde ExeLoader: exception table merge for Wine skipped; only the game's functions have unwind data");
    }
    if (ThreadNameHandlerWanted() && AddVectoredExceptionHandler(1, &ResumeAfterThreadName))
        s_threadNameHandlerInstalled = true;
#endif

    const size_t ntCompleteHeaderSize = sizeof(IMAGE_NT_HEADERS) + (ntHeader->FileHeader.NumberOfSections * (sizeof(IMAGE_SECTION_HEADER)));

    // overwrite our headers with the target headers
    std::memcpy(sourceNtHeader, ntHeader, ntCompleteHeaderSize);

    // good old switcheroo
    // TODO: consider making this optional to allow loading the game's pdb.
    sourceNtHeader->OptionalHeader.CheckSum = sourceChecksum;
    sourceNtHeader->FileHeader.TimeDateStamp = sourceTimestamp;
    sourceNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG] = sourceDebugDir;
    sourceNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = sourceExportDir;
    m_pBinary = nullptr;

    // Set a hook to check if anything loaded messes with critical hooks.
    extern void HookFormAllocateSentinelInit();
    HookFormAllocateSentinelInit();

    return true;
}
