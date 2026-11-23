#pragma once

#include <cstdint>

//! Whether a player may add to or take from a shared container (one with no owner): only from a game that has it in range,
//! and by a count a container could hold. Pure rules on plain values, so they can be tested (tests/container_trust.cpp).
namespace ContainerTrust
{
//! The most of one item a single change may add or take. A stack is a game's count; this is far above any honest one.
constexpr int64_t kMaxChange = 1000000;

//! aInRange: the sender's loaded grid holds the container's cell.
//! aCount: the change, negative for a take.
inline bool MayChange(const bool aInRange, const int32_t aCount) noexcept
{
    if (!aInRange || aCount == 0)
        return false;
    const int64_t cCount = aCount;
    return (cCount < 0 ? -cCount : cCount) <= kMaxChange;
}
} // namespace ContainerTrust
