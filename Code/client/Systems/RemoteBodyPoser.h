#pragma once

#include <cstdint>

// Which code poses a remote VR player's copy on a flat screen. SEBodyPresentation commits the copies that hold a slot
// (RemoteBodySlots) natively, inside the engine's update; VRBodySync::OnFrameEnd poses every other one. Each commit's
// actor and engine frame are one word, so a frame-end reader sees both together.
namespace RemoteBodyPoser
{
[[nodiscard]] inline uint64_t CommitWord(const uint32_t aActor, const uint32_t aFrame) noexcept
{
    return static_cast<uint64_t>(aActor) << 32 | aFrame;
}

// The native commit poses aActor while it committed it in aFrame or the frame before; older, frame end poses it again.
[[nodiscard]] inline bool NativeCommitPoses(const uint64_t aWord, const uint32_t aActor, const uint32_t aFrame) noexcept
{
    return aActor && static_cast<uint32_t>(aWord >> 32) == aActor && aFrame - static_cast<uint32_t>(aWord) <= 1;
}

// The word once aActor's copy had a native update the commit did not follow (0: every copy): frame end poses it from
// that same frame on. A word for another copy is kept.
[[nodiscard]] inline uint64_t Withdrawn(const uint64_t aWord, const uint32_t aActor) noexcept
{
    return !aActor || static_cast<uint32_t>(aWord >> 32) == aActor ? 0 : aWord;
}
} // namespace RemoteBodyPoser
