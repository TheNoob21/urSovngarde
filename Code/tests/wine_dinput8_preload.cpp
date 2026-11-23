// Whether the launcher preloads System32's dinput8.dll (immersive_launcher/SystemPreload.h): on Windows as always, and
// not under Wine, where the game's DINPUT8 import bound to Wine's builtin dinput8.dll ended the process in SKSE VR's input
// set-up. URSOVNGARDE_PRELOAD_SYSTEM_DINPUT8 overrides either way.

#include <catch2/catch.hpp>

#include <immersive_launcher/SystemPreload.h>

#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <iterator>

using system_preload::ChooseDinput8Preload;
using system_preload::Describe;
using system_preload::Dinput8Choice;
using system_preload::Dinput8Reason;

// The choice is made before anything else runs in main(), so it must not need anything at run time.
static_assert(ChooseDinput8Preload(false, nullptr).Preload);
static_assert(!ChooseDinput8Preload(true, nullptr).Preload);

TEST_CASE("Windows preloads System32's dinput8.dll as before, and Wine leaves it out", "[wine.dinput8]")
{
    const Dinput8Choice cWindows = ChooseDinput8Preload(false, nullptr);
    CHECK(cWindows.Preload);
    CHECK(cWindows.Reason == Dinput8Reason::kNotWine);

    const Dinput8Choice cWine = ChooseDinput8Preload(true, nullptr);
    CHECK_FALSE(cWine.Preload);
    CHECK(cWine.Reason == Dinput8Reason::kWine);
}

TEST_CASE("URSOVNGARDE_PRELOAD_SYSTEM_DINPUT8 decides on Windows and under Wine alike", "[wine.dinput8]")
{
    for (const bool cUnderWine : {false, true})
    {
        INFO("under Wine: " << cUnderWine);

        for (const char* pOff : {"0", "off", "OFF", "False", "no", "No"})
        {
            INFO("setting: " << pOff);
            const Dinput8Choice cChoice = ChooseDinput8Preload(cUnderWine, pOff);
            CHECK_FALSE(cChoice.Preload);
            CHECK(cChoice.Reason == Dinput8Reason::kSettingOff);
        }

        // Any other value preloads, as any value but an off word turns the loader's other switches on.
        for (const char* pOn : {"1", "on", "TRUE", "yes", "2", "00", "0 ", "offf", "nope", "preload"})
        {
            INFO("setting: " << pOn);
            const Dinput8Choice cChoice = ChooseDinput8Preload(cUnderWine, pOn);
            CHECK(cChoice.Preload);
            CHECK(cChoice.Reason == Dinput8Reason::kSettingOn);
        }
    }
}

TEST_CASE("An empty URSOVNGARDE_PRELOAD_SYSTEM_DINPUT8 counts as absent", "[wine.dinput8]")
{
    CHECK(ChooseDinput8Preload(false, "").Reason == Dinput8Reason::kNotWine);
    CHECK(ChooseDinput8Preload(true, "").Reason == Dinput8Reason::kWine);
    CHECK_FALSE(ChooseDinput8Preload(true, "").Preload);
}

TEST_CASE("Each choice has its own log line about System32's dinput8.dll", "[wine.dinput8]")
{
    const Dinput8Choice cChoices[] = {{true, Dinput8Reason::kNotWine}, {false, Dinput8Reason::kWine}, {true, Dinput8Reason::kSettingOn}, {false, Dinput8Reason::kSettingOff}};

    for (size_t i = 0; i < std::size(cChoices); ++i)
    {
        const char* pLine = Describe(cChoices[i]);
        INFO("line: " << pLine);
        REQUIRE(pLine != nullptr);
        CHECK(std::strstr(pLine, "System32 dinput8.dll") != nullptr);
        CHECK(std::strchr(pLine, '\n') == nullptr);
        // A line says "skipped" exactly when the preload is left out.
        CHECK((std::strstr(pLine, "skipped") != nullptr) == !cChoices[i].Preload);

        for (size_t j = 0; j < i; ++j)
            CHECK(std::strcmp(pLine, Describe(cChoices[j])) != 0);
    }
}
