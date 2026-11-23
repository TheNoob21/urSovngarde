#pragma once

#include <common/DInputSetting.h>

// Under Wine, let the game resolve its DirectInput proxy before the system DLL.
// URSOVNGARDE_PRELOAD_SYSTEM_DINPUT8 overrides that choice when set.

namespace system_preload
{
enum class Dinput8Reason
{
    kNotWine,    // preloaded: not Wine
    kWine,       // left out: Wine
    kSettingOn,  // preloaded: URSOVNGARDE_PRELOAD_SYSTEM_DINPUT8 says so
    kSettingOff, // left out: URSOVNGARDE_PRELOAD_SYSTEM_DINPUT8 says so
};

struct Dinput8Choice
{
    bool Preload;
    Dinput8Reason Reason;
};

constexpr Dinput8Choice ChooseDinput8Preload(bool aUnderWine, const char* apSetting) noexcept
{
    if (apSetting && *apSetting)
    {
        const bool cOff = DInputSetting::IsOff(apSetting);
        return cOff ? Dinput8Choice{false, Dinput8Reason::kSettingOff} : Dinput8Choice{true, Dinput8Reason::kSettingOn};
    }
    return aUnderWine ? Dinput8Choice{false, Dinput8Reason::kWine} : Dinput8Choice{true, Dinput8Reason::kNotWine};
}

// The line for the logs (no newline).
constexpr const char* Describe(const Dinput8Choice& acChoice) noexcept
{
    switch (acChoice.Reason)
    {
    case Dinput8Reason::kNotWine: return "urSovngarde launcher: System32 dinput8.dll preloaded (not Wine)";
    case Dinput8Reason::kWine: return "urSovngarde launcher: System32 dinput8.dll preload skipped (Wine), the game's DINPUT8 import takes the normal DLL search";
    case Dinput8Reason::kSettingOn: return "urSovngarde launcher: System32 dinput8.dll preloaded (URSOVNGARDE_PRELOAD_SYSTEM_DINPUT8 on)";
    case Dinput8Reason::kSettingOff: return "urSovngarde launcher: System32 dinput8.dll preload skipped (URSOVNGARDE_PRELOAD_SYSTEM_DINPUT8 off)";
    }
    return "urSovngarde launcher: System32 dinput8.dll preload choice unknown";
}
} // namespace system_preload
