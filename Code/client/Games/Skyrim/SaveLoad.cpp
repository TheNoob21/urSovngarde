#include <TiltedOnlinePCH.h>

#include <SaveLoad.h>
#include <LoadLatch.h>
#include <mutex>
#include <Helpers/ModuleImage.h>

namespace PlayerSaveContext
{
namespace
{
std::recursive_mutex s_lock;
LoadLatch s_latch;

uint64_t CurrentPlayer() noexcept
{
    // CommonLibSSE-NG BGSSaveLoadManager::currentPlayerID, +0xD0.
    // CommonLibVR's manager has the same member position (named unk0D0 there).
    // The singleton itself is independent of the change-flags manager (516851).
    POINTER_SKYRIMSE(BGSSaveLoadManager*, s_manager, 403340, 516860);
    BGSSaveLoadManager* pManager = s_manager.Get() ? *s_manager.Get() : nullptr;
    return pManager ? *reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(pManager) + 0xD0) : 0;
}
} // namespace

Snapshot Read() noexcept
{
    std::scoped_lock lock(s_lock);
    return {s_latch.Loading() ? 0 : CurrentPlayer(), s_latch.Generation(), s_latch.Loading()};
}

uint64_t BeginLoad() noexcept
{
    std::scoped_lock lock(s_lock);
    s_latch.Begin();
    return s_latch.Generation();
}

void RefuseLoad(const uint64_t aGeneration) noexcept
{
    std::scoped_lock lock(s_lock);
    s_latch.Refused(aGeneration);
}

void CompleteLoad() noexcept
{
    std::scoped_lock lock(s_lock);
    s_latch.Complete();
}

bool RunFor(const Snapshot& acExpected, const std::function<void()>& acWrite) noexcept
{
    std::scoped_lock lock(s_lock);
    if (s_latch.Loading() || acExpected.Loading || !acExpected.PlayerId || s_latch.Generation() != acExpected.Generation || CurrentPlayer() != acExpected.PlayerId)
        return false;
    acWrite();
    return true;
}
} // namespace PlayerSaveContext

// A save load blocks the party words until its load event; a request the game refuses sends none.
TP_THIS_FUNCTION(TSaveLoadManagerLoad, bool, BGSSaveLoadManager, const char* apFileName, int32_t aDeviceId, uint32_t aOutputStats, bool aCheckForMods);
static TSaveLoadManagerLoad* RealSaveLoadManagerLoad = nullptr;

bool TP_MAKE_THISCALL(HookSaveLoadManagerLoad, BGSSaveLoadManager, const char* apFileName, int32_t aDeviceId, uint32_t aOutputStats, bool aCheckForMods)
{
    spdlog::debug(
        "SaveLoad: load requested for '{}' (device {}, stats {}, check mods {}) from {}", apFileName ? apFileName : "(null)", aDeviceId, aOutputStats, aCheckForMods,
        ModuleImage::Where(_ReturnAddress()));
    const uint64_t cLoadGeneration = PlayerSaveContext::BeginLoad();
    const bool result = TiltedPhoques::ThisCall(RealSaveLoadManagerLoad, apThis, apFileName, aDeviceId, aOutputStats, aCheckForMods);
    spdlog::debug("SaveLoad: load of '{}' returned {}", apFileName ? apFileName : "(null)", result);
    if (!result)
        PlayerSaveContext::RefuseLoad(cLoadGeneration);
    return result;
}

static TiltedPhoques::Initializer s_saveLoadHooks(
    []()
    {
        POINTER_SKYRIMSE(TSaveLoadManagerLoad, s_load, 35728, 34819);
        if (s_load.Get())
        {
            RealSaveLoadManagerLoad = s_load.Get();
            TP_HOOK(&RealSaveLoadManagerLoad, HookSaveLoadManagerLoad);
        }
        else
            spdlog::warn("SaveLoad: id 34819 not resolved, a refused load is not seen");
    });

void BGSSaveLoadManager::Save(SaveData* apData)
{
    apData->flags |= 4;

    const char* cSaveName = "";
    if (apData->saveName)
        cSaveName = apData->saveName;
}
