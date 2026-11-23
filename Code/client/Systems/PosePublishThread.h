#pragma once

#include <cstdint>

// Which thread may hand a remote player's pose to the flat body commit (SEBodyPresentation).
// The client's update runs inside the game's script VM update, which the game hands to any of its job threads, so the
// thread changes from frame to frame (six of them on a PCVR setup). Publication used to belong to the first
// thread that published, for good: the other threads' updates were skipped, so the body got a new pose on one frame in
// six, about 10 times a second, and held it in between. Now the thread of each client update owns it until the next one.
namespace PosePublishThread
{
// The owner when a pose is published on aCurrent: none yet makes aCurrent the owner.
[[nodiscard]] inline uint32_t OwnerForPublish(const uint32_t aOwner, const uint32_t aCurrent) noexcept
{
    return aOwner ? aOwner : aCurrent;
}

[[nodiscard]] inline bool MayPublish(const uint32_t aOwner, const uint32_t aCurrent) noexcept
{
    return aOwner != 0 && aOwner == aCurrent;
}
} // namespace PosePublishThread
