// How the DirectInput hook reaches IDirectInput8A::CreateDevice (Libraries/overrides/TiltedHooks/DInputHookMode.h): by
// patching its code on Windows as TiltedHooks always did, and in its vtable slot under Wine, where the code patch wrote an
// x64 jump over Wine's native ARM64 CreateDevice. URSOVNGARDE_DINPUT_VTABLE_HOOK overrides either way.

#include <catch2/catch.hpp>

#include "../../Libraries/overrides/TiltedHooks/DInputHookMode.h"

#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <iterator>

using dinput_hook_mode::ChooseCreateDeviceHook;
using dinput_hook_mode::Describe;
using dinput_hook_mode::HookChoice;
using dinput_hook_mode::HookMode;
using dinput_hook_mode::HookReason;

// Install() decides before the game's first DirectInput8Create, so the choice must not need anything at run time.
static_assert(ChooseCreateDeviceHook(false, nullptr).Mode == HookMode::kCodePatch);
static_assert(ChooseCreateDeviceHook(true, nullptr).Mode == HookMode::kVtableSlot);

TEST_CASE("Windows patches CreateDevice's code as before, and Wine hooks its vtable slot", "[wine.dinput8.hook]")
{
    const HookChoice cWindows = ChooseCreateDeviceHook(false, nullptr);
    CHECK(cWindows.Mode == HookMode::kCodePatch);
    CHECK(cWindows.Reason == HookReason::kNotWine);

    const HookChoice cWine = ChooseCreateDeviceHook(true, nullptr);
    CHECK(cWine.Mode == HookMode::kVtableSlot);
    CHECK(cWine.Reason == HookReason::kWine);
}

TEST_CASE("URSOVNGARDE_DINPUT_VTABLE_HOOK decides on Windows and under Wine alike", "[wine.dinput8.hook]")
{
    for (const bool cUnderWine : {false, true})
    {
        INFO("under Wine: " << cUnderWine);

        for (const char* pOff : {"0", "off", "OFF", "False", "no", "No"})
        {
            INFO("setting: " << pOff);
            const HookChoice cChoice = ChooseCreateDeviceHook(cUnderWine, pOff);
            CHECK(cChoice.Mode == HookMode::kCodePatch);
            CHECK(cChoice.Reason == HookReason::kSettingOff);
        }

        // Any other value swaps the slot, as any value but an off word turns the launcher's switches on.
        for (const char* pOn : {"1", "on", "TRUE", "yes", "2", "00", "0 ", "offf", "nope", "vtable"})
        {
            INFO("setting: " << pOn);
            const HookChoice cChoice = ChooseCreateDeviceHook(cUnderWine, pOn);
            CHECK(cChoice.Mode == HookMode::kVtableSlot);
            CHECK(cChoice.Reason == HookReason::kSettingOn);
        }
    }
}

TEST_CASE("An empty URSOVNGARDE_DINPUT_VTABLE_HOOK counts as absent", "[wine.dinput8.hook]")
{
    CHECK(ChooseCreateDeviceHook(false, "").Reason == HookReason::kNotWine);
    CHECK(ChooseCreateDeviceHook(true, "").Reason == HookReason::kWine);
    CHECK(ChooseCreateDeviceHook(true, "").Mode == HookMode::kVtableSlot);
}

TEST_CASE("Each CreateDevice hook choice has its own log line", "[wine.dinput8.hook]")
{
    const HookChoice cChoices[] = {
        {HookMode::kCodePatch, HookReason::kNotWine},
        {HookMode::kVtableSlot, HookReason::kWine},
        {HookMode::kVtableSlot, HookReason::kSettingOn},
        {HookMode::kCodePatch, HookReason::kSettingOff}};

    for (size_t i = 0; i < std::size(cChoices); ++i)
    {
        const char* pLine = Describe(cChoices[i]);
        INFO("line: " << pLine);
        REQUIRE(pLine != nullptr);
        CHECK(std::strstr(pLine, "CreateDevice") != nullptr);
        CHECK(std::strchr(pLine, '\n') == nullptr);
        // A line says "vtable slot" exactly when the slot is used.
        CHECK((std::strstr(pLine, "vtable slot") != nullptr) == (cChoices[i].Mode == HookMode::kVtableSlot));

        for (size_t j = 0; j < i; ++j)
            CHECK(std::strcmp(pLine, Describe(cChoices[j])) != 0);
    }
}

#ifdef _WIN32
#include <windows.h>

namespace
{
// A COM-shaped object: its first member points at a table of functions shared by every object of the kind, in read-only
// memory as a DLL's vtable is (Wine's IDirectInput8 vtable sits in dinput8.dll's .rdata).
struct FakeObject;
using TCreate = HRESULT(__stdcall*)(FakeObject* apThis, int aValue, int* apOut);

struct FakeVtbl
{
    void* Slots[4]; // QueryInterface, AddRef, Release, then "CreateDevice"
};

struct FakeObject
{
    const FakeVtbl* lpVtbl;
    int OriginalCalls;
};

constexpr size_t kCreateSlot = 3;

int s_hookCalls = 0;
TCreate s_realCreate = nullptr;

HRESULT __stdcall OriginalCreate(FakeObject* apThis, int aValue, int* apOut)
{
    ++apThis->OriginalCalls;
    *apOut = aValue * 2;
    return S_OK;
}

HRESULT __stdcall HookCreate(FakeObject* apThis, int aValue, int* apOut)
{
    ++s_hookCalls;
    const HRESULT cResult = s_realCreate(apThis, aValue, apOut);
    *apOut += 1;
    return cResult;
}

HRESULT CallCreate(FakeObject& aObject, int aValue, int* apOut)
{
    return reinterpret_cast<TCreate>(aObject.lpVtbl->Slots[kCreateSlot])(&aObject, aValue, apOut);
}

DWORD ProtectionOf(const void* apAddress)
{
    MEMORY_BASIC_INFORMATION info{};
    REQUIRE(VirtualQuery(apAddress, &info, sizeof(info)) == sizeof(info));
    return info.Protect;
}

// A page holding one FakeVtbl, then given acProtect.
struct VtablePage
{
    explicit VtablePage(DWORD aProtect)
    {
        pMemory = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        REQUIRE(pMemory != nullptr);
        pVtbl = static_cast<FakeVtbl*>(pMemory);
        for (void*& pSlot : pVtbl->Slots)
            pSlot = nullptr;
        pVtbl->Slots[kCreateSlot] = reinterpret_cast<void*>(&OriginalCreate);
        DWORD old = 0;
        REQUIRE(VirtualProtect(pMemory, 4096, aProtect, &old));
    }
    ~VtablePage() { VirtualFree(pMemory, 0, MEM_RELEASE); }

    void* pMemory = nullptr;
    FakeVtbl* pVtbl = nullptr;
};
} // namespace

TEST_CASE("A swapped vtable slot routes calls through the hook and on to the original", "[wine.dinput8.hook]")
{
    s_hookCalls = 0;
    s_realCreate = nullptr;
    VtablePage page(PAGE_READONLY);
    FakeObject first{page.pVtbl, 0};
    FakeObject second{page.pVtbl, 0};

    // The original's code, to see that the swap leaves it alone (the code patch is what broke Wine's ARM64 code).
    unsigned char codeBefore[16];
    std::memcpy(codeBefore, reinterpret_cast<const void*>(&OriginalCreate), sizeof(codeBefore));

    int out = 0;
    REQUIRE(CallCreate(first, 5, &out) == S_OK);
    CHECK(out == 10);
    CHECK(s_hookCalls == 0);

    auto** ppSlot = &page.pVtbl->Slots[kCreateSlot];
    REQUIRE(dinput_hook_mode::SwapVtableSlot(ppSlot, reinterpret_cast<void*>(&HookCreate), reinterpret_cast<void**>(&s_realCreate)));
    CHECK(s_realCreate == &OriginalCreate);
    CHECK(*ppSlot == reinterpret_cast<void*>(&HookCreate));

    // The page is read-only again, as the DLL's .rdata was.
    CHECK(ProtectionOf(ppSlot) == PAGE_READONLY);

    // Through the hook, which calls the original: both objects share the table, so both are hooked.
    REQUIRE(CallCreate(first, 5, &out) == S_OK);
    CHECK(out == 11);
    CHECK(first.OriginalCalls == 2);
    REQUIRE(CallCreate(second, 7, &out) == S_OK);
    CHECK(out == 15);
    CHECK(second.OriginalCalls == 1);
    CHECK(s_hookCalls == 2);

    CHECK(std::memcmp(codeBefore, reinterpret_cast<const void*>(&OriginalCreate), sizeof(codeBefore)) == 0);
}

TEST_CASE("Swapping a slot that already holds the hook changes nothing", "[wine.dinput8.hook]")
{
    s_hookCalls = 0;
    s_realCreate = nullptr;
    VtablePage page(PAGE_READONLY);
    FakeObject object{page.pVtbl, 0};
    auto** ppSlot = &page.pVtbl->Slots[kCreateSlot];

    REQUIRE(dinput_hook_mode::SwapVtableSlot(ppSlot, reinterpret_cast<void*>(&HookCreate), reinterpret_cast<void**>(&s_realCreate)));
    // A second swap must not take the hook for the original: the hook would then call itself.
    CHECK_FALSE(dinput_hook_mode::SwapVtableSlot(ppSlot, reinterpret_cast<void*>(&HookCreate), reinterpret_cast<void**>(&s_realCreate)));
    CHECK(s_realCreate == &OriginalCreate);
    CHECK(ProtectionOf(ppSlot) == PAGE_READONLY);

    int out = 0;
    REQUIRE(CallCreate(object, 1, &out) == S_OK);
    CHECK(out == 3);
    CHECK(s_hookCalls == 1);
    CHECK(object.OriginalCalls == 1);
}

TEST_CASE("A vtable slot on an executable page keeps the page executable", "[wine.dinput8.hook]")
{
    s_hookCalls = 0;
    s_realCreate = nullptr;
    VtablePage page(PAGE_EXECUTE_READ);
    auto** ppSlot = &page.pVtbl->Slots[kCreateSlot];

    REQUIRE(dinput_hook_mode::SwapVtableSlot(ppSlot, reinterpret_cast<void*>(&HookCreate), reinterpret_cast<void**>(&s_realCreate)));
    CHECK(s_realCreate == &OriginalCreate);
    CHECK(ProtectionOf(ppSlot) == PAGE_EXECUTE_READ);
}

TEST_CASE("A slot in memory that is not committed is left alone", "[wine.dinput8.hook]")
{
    void* pReserved = VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS);
    REQUIRE(pReserved != nullptr);

    TCreate original = &OriginalCreate;
    CHECK_FALSE(dinput_hook_mode::SwapVtableSlot(static_cast<void**>(pReserved), reinterpret_cast<void*>(&HookCreate), reinterpret_cast<void**>(&original)));
    CHECK(original == &OriginalCreate);

    VirtualFree(pReserved, 0, MEM_RELEASE);
}
#endif
