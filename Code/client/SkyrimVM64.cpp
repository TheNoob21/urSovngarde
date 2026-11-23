#include <Actor.h>
#include <Games/Skyrim/VR/VRBodySync.h>
#include <Games/Skyrim/Watches/BoneHold.h>
#include <Games/Skyrim/Watches/EyeBoneWatch.h>
#include <Games/Skyrim/PropHavok.h>
#include <Services/CharacterService.h>
#include <Services/FurnitureService.h>
#include <Services/MountService.h>
#include <World.h>
#include <TiltedOnlinePCH.h>
#include <Services/GameExtension.h>
#include "TiltedOnlineApp.h"
#include <Misc/GameVM.h>
#include <Interface/Menus/SleepWaitOk.h>
#include <Interface/Menus/SleepWaitVoteLine.h>
#include <Interface/Menus/SkillsMenu.h>
#include <Interface/Menus/MapMenu.h>
#include <Interface/UI.h>
#include <Helpers/PatchCheck.h>
#include <StuckArrows.h>
#include <Services/ExtensionService.h>

extern std::unique_ptr<TiltedOnlineApp> g_appInstance;

struct Main;

TP_THIS_FUNCTION(TVMUpdate, int, GameVM, float);
TP_THIS_FUNCTION(TMainLoop, short, Main);
TP_THIS_FUNCTION(TVMDestructor, uintptr_t, void);

static TVMUpdate* VMUpdate = nullptr;
static TMainLoop* MainLoop = nullptr;
static TVMDestructor* VMDestructor = nullptr;

#ifdef SKYRIMVR
// The main loop's call to Main::Update, the only one in the game.
constexpr uintptr_t kMainUpdateCall = 0x5B6E8F;
#endif

// Main::Update itself, or another plugin's swap of this call (code outside the game's image).
static bool IsMainUpdateCall(const uint8_t* apCall, const void* apMainUpdate)
{
    const auto cImage = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* pHeaders = reinterpret_cast<const IMAGE_NT_HEADERS64*>(cImage + reinterpret_cast<const IMAGE_DOS_HEADER*>(cImage)->e_lfanew);
    return PatchCheck::IsCallTo(apCall, apMainUpdate, cImage, cImage + pHeaders->OptionalHeader.SizeOfImage);
}

int TP_MAKE_THISCALL(HookVMUpdate, GameVM, float a2)
{
    if (apThis->inactive == 0)
        g_appInstance->Update();

    return TiltedPhoques::ThisCall(VMUpdate, apThis, a2);
}

short TP_MAKE_THISCALL(HookMainLoop, Main)
{
    if (entt::locator<World>::has_value())
        World::Get().ctx().at<ExtensionService>().Update();
    StuckArrows::OnMainLoop();
#ifdef SKYRIMVR
    UnpausedMenusOnMainLoop();
    UpdateSkillsMenuChecks();
    RunDeferredSkillsTreeUpdate();
    ReleaseClosedSkillsTrees();
    RunHeldMapHide();
    VRBodySync::AddPendingWeaponBodies();
    if (entt::locator<World>::has_value())
        World::Get().GetCharacterService().LayDownStandingBodies();
    // On the main thread between frames: the client's own update may run on another thread during a load.
    BoneHold::Update();
    EyeBoneWatch::Update();
#endif
    // Between frames, outside the world's step: copies' animation steps, and copies' ragdolls the flat game moves to where
    // their owners' bodies lie.
    Actor::RunQueuedCopyAnimations();
    VRBodySync::MovePendingRagdolls();
    // The shared sleep's agreed OK and the vote's line in the menu: on the main thread, before the frame's update runs the menus.
    SleepWaitOk::PressAgreedOk();
    SleepWaitVoteLine::Update();
    CharacterService::DisableQueuedCopies();
    // Raises another game's bodies, decided by the client's update, which may run on another thread.
    CharacterService::MakeDecidedRaises();
    // Copies put on and taken off horses, and seated and stood, as the client's last update decided.
    MountService::RunQueuedWork();
    FurnitureService::RunQueuedWork();
    GameExtension::OnGameStep();
    // Actors held standing by another player's hand, stepped as the client's last update worked out.
    // Flat: the hanging bodies of placed objects another player swings, queued by the client's update.
    PropHavok::RunQueued();

    return TiltedPhoques::ThisCall(MainLoop, apThis);
}

uintptr_t TP_MAKE_THISCALL(HookVMDestructor, void)
{
    TP_EMPTY_HOOK_PLACEHOLDER

    return TiltedPhoques::ThisCall(VMDestructor, apThis);
}

static TiltedPhoques::Initializer s_mainHooks(
    []()
    {
        POINTER_SKYRIMSE(TVMUpdate, cVMUpdate, 53926, 53926);
        VMUpdate = cVMUpdate.Get();
        TP_HOOK(&VMUpdate, HookVMUpdate);

#ifdef SKYRIMVR
        // VR needs Main::Update for the shared sleep's agreed OK and its line in the menu; without it the OK button works as in the game and time is not shared.
        // Its call is swapped and the function left as it is: CBPC detours Main::Update, following a jump at its start and copying the bytes it finds there without relocating them.
        // With the function detoured, CBPC's copy held our hook's first call, which then landed in the heap and crashed the game at the main menu.
        POINTER_SKYRIMSE(TMainLoop, cMainLoop, 36564, 36564);
        auto* pMainUpdateCall = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + kMainUpdateCall);
        if (IsMainUpdateCall(pMainUpdateCall, reinterpret_cast<const void*>(cMainLoop.Get())))
        {
            TiltedPhoques::SwapCall(pMainUpdateCall, MainLoop, &HookMainLoop);
            SleepWaitOk::EnableHold();
        }
        else
            spdlog::error("Main loop: {} is not the call to Main::Update ({}), leaving it alone; the Sleep/Wait OK works as in the game and time is not shared",
                          fmt::ptr(pMainUpdateCall), PatchCheck::Bytes(pMainUpdateCall, 5));
#else
        // CBPC detours Main::Update on SE too, so swap the main loop's call here as well.
        POINTER_SKYRIMSE(TMainLoop, cMainLoop, 36564, 36564);
        MainLoop = cMainLoop.Get();
        const VersionDbPtr<uint8_t> mainLoopCaller(36544);
        auto* pMainUpdateCall = PatchCheck::At(mainLoopCaller.Get(), 0x160);
        if (IsMainUpdateCall(pMainUpdateCall, reinterpret_cast<const void*>(MainLoop)))
        {
            TiltedPhoques::SwapCall(pMainUpdateCall, MainLoop, &HookMainLoop);
            SleepWaitOk::EnableHold();
        }
        else
            spdlog::error(
                "Main loop: {} is not the call to Main::Update ({}), leaving it alone; the per-frame work between frames does not run and time is not shared",
                fmt::ptr(pMainUpdateCall), PatchCheck::Bytes(pMainUpdateCall, 5));

        // An empty placeholder, and the address does not exist on VR.
        POINTER_SKYRIMSE(TVMDestructor, cVMDestructor, 40412, 40412);
        VMDestructor = cVMDestructor.Get();
        TP_HOOK(&VMDestructor, HookVMDestructor);
#endif
    });

