#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// Whether a hand of a remote player's copy is where the player's game has it this frame (posed from their VR pose),
// rather than where its own animation puts it. Only then may an object be drawn from that hand: an animation hand can
// sit still next to a slowly carried object and look as if it held it.
// The code that poses copies registers the question; with nothing registered no hand counts as posed. aSide: 0 left,
// 1 right.
namespace PosedHands
{
using Query = bool (*)(uint32_t aActorFormId, size_t aSide) noexcept;

inline std::atomic<Query> g_query{nullptr};

inline void SetQuery(const Query acQuery) noexcept
{
    g_query.store(acQuery, std::memory_order_release);
}

inline bool IsPosed(const uint32_t aActorFormId, const size_t aSide) noexcept
{
    const Query cQuery = g_query.load(std::memory_order_acquire);
    return cQuery && cQuery(aActorFormId, aSide);
}
} // namespace PosedHands
