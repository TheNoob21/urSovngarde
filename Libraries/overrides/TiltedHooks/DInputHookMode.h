#pragma once

#include <common/DInputSetting.h>

// Wine can expose native ARM64 code through an x64 DirectInput vtable. Hook the slot to preserve that code.
// URSOVNGARDE_DINPUT_VTABLE_HOOK overrides the default hook mode when set.

#ifdef _WIN32
#include <windows.h>
#endif

namespace dinput_hook_mode
{
enum class HookMode
{
    kCodePatch,  // TP_HOOK_IMMEDIATE over CreateDevice's code, as TiltedHooks always did
    kVtableSlot, // CreateDevice's vtable entry points at the hook; no code changes
};

enum class HookReason
{
    kNotWine,    // code patch: not Wine
    kWine,       // vtable slot: Wine
    kSettingOn,  // vtable slot: URSOVNGARDE_DINPUT_VTABLE_HOOK says so
    kSettingOff, // code patch: URSOVNGARDE_DINPUT_VTABLE_HOOK says so
};

struct HookChoice
{
    HookMode Mode;
    HookReason Reason;
};

constexpr HookChoice ChooseCreateDeviceHook(bool aUnderWine, const char* apSetting) noexcept
{
    if (apSetting && *apSetting)
    {
        const bool cOff = DInputSetting::IsOff(apSetting);
        return cOff ? HookChoice{HookMode::kCodePatch, HookReason::kSettingOff} : HookChoice{HookMode::kVtableSlot, HookReason::kSettingOn};
    }
    return aUnderWine ? HookChoice{HookMode::kVtableSlot, HookReason::kWine} : HookChoice{HookMode::kCodePatch, HookReason::kNotWine};
}

// The line for tp_client.log (no newline).
constexpr const char* Describe(const HookChoice& acChoice) noexcept
{
    switch (acChoice.Reason)
    {
    case HookReason::kNotWine: return "DInputHook: CreateDevice is hooked by patching its code (not Wine)";
    case HookReason::kWine: return "DInputHook: CreateDevice is hooked in its vtable slot (Wine), its code is left alone";
    case HookReason::kSettingOn: return "DInputHook: CreateDevice is hooked in its vtable slot (URSOVNGARDE_DINPUT_VTABLE_HOOK on), its code is left alone";
    case HookReason::kSettingOff: return "DInputHook: CreateDevice is hooked by patching its code (URSOVNGARDE_DINPUT_VTABLE_HOOK off)";
    }
    return "DInputHook: CreateDevice hook choice unknown";
}

#ifdef _WIN32
// Points the vtable entry at appSlot to apHook, and returns whether it did. What the slot held goes to *appOriginal before
// the slot changes, so a call through the slot from another thread already finds it there.
// The slot's page (read-only image data for a COM vtable) is made writable only for the write and gets its protection back,
// executable or not. Nothing changes and false comes back when the slot is not committed memory, cannot be made writable,
// or already holds apHook (so a second call never takes the hook for the original and calls itself).
inline bool SwapVtableSlot(void** appSlot, void* apHook, void** appOriginal) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(appSlot, &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT)
        return false;

    constexpr DWORD cExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const DWORD cWritable = (info.Protect & cExecutable) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    DWORD oldProtect = 0;
    if (!VirtualProtect(appSlot, sizeof(void*), cWritable, &oldProtect))
        return false;

    void* const pIncomingOriginal = *appOriginal;
    void* pCurrent = *static_cast<void* volatile*>(appSlot);
    bool swapped = false;
    while (pCurrent != apHook)
    {
        *appOriginal = pCurrent;
        void* const pSeen = InterlockedCompareExchangePointer(appSlot, apHook, pCurrent);
        if (pSeen == pCurrent)
        {
            swapped = true;
            break;
        }
        pCurrent = pSeen;
    }
    if (!swapped)
        *appOriginal = pIncomingOriginal;

    DWORD ignored = 0;
    VirtualProtect(appSlot, sizeof(void*), oldProtect, &ignored);
    return swapped;
}
#endif
} // namespace dinput_hook_mode
