#pragma once

#include <CopyWork.h>

#include <chrono>

#include <Games/Memory.h>
#include <Interface/UI.h>
#include <TESObjectREFR.h>

//! Whether a loading screen is up now.
inline bool LoadingScreenUp() noexcept
{
    static const BSFixedString s_loading("Loading Menu");
    const UI* pUI = UI::Get();
    return pUI && pUI->GetMenuOpen(s_loading);
}

//! The game's load state now (CopyWork::Recheck).
inline CopyWork::Moment LoadMomentNow() noexcept
{
    return {IsProcessExiting(), LoadingScreenUp(), UI::LoadingScreensQueued()};
}

//! CopyWork::Settled for the game as it is now. aGate is the caller's own, kept between its passes.
inline bool CopyWorkSettled(CopyWork::LoadGate& aGate) noexcept
{
    const auto cNowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    return CopyWork::Settled(aGate, IsProcessExiting(), LoadingScreenUp(), UI::LoadingScreensQueued(), cNowMs);
}

/**
 * @brief A reference looked up by handle and held for as long as this lives, so that the game cannot free it while work is done on it.
 *
 * TESObjectREFR::GetByHandle gives the reference's hold back before it returns.
 */
struct RetainedReference
{
    explicit RetainedReference(const uint32_t aHandle) noexcept
    {
        if (!aHandle)
            return;
        using TGetRefrByHandle = void(uint32_t& aHandle, TESObjectREFR*& apResult);
        POINTER_SKYRIMSE(TGetRefrByHandle, s_getRefrByHandle, 17201, 12204);
        uint32_t handle = aHandle;
        s_getRefrByHandle.Get()(handle, m_pReference);
    }

    ~RetainedReference() noexcept
    {
        if (m_pReference)
            m_pReference->handleRefObject.DecRefHandle();
    }

    RetainedReference(const RetainedReference&) = delete;
    RetainedReference& operator=(const RetainedReference&) = delete;

    [[nodiscard]] TESObjectREFR* Get() const noexcept { return m_pReference; }

private:
    TESObjectREFR* m_pReference{};
};
