#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <TiltedCore/Stl.hpp>

// Magic effects held back until their target and caster are known here, with no game in them.
namespace EffectQueue
{
// One round over a queue of held-back effects: each effect in it at the start is looked at once, in order.
// aTake(effect, aTargetWaiting) says whether to keep it: false once it is applied or has expired, true while it still waits.
// A kept effect goes back with its own expiry, behind the others, and the round goes on, so one effect waiting for a copy that
// is not here yet holds back no other target's. aTargetWaiting says an earlier effect on the same target (aTargetOf) is kept this
// round: that effect waits too, so a target still takes its effects in order. Effects queued during the round wait for the next.
template <class Queue, class TargetOf, class Take> void Round(Queue& aQueue, TargetOf&& aTargetOf, Take&& aTake)
{
    TiltedPhoques::Vector<uint32_t> waiting;
    for (size_t left = aQueue.size(); left > 0; --left)
    {
        auto effect = std::move(aQueue.front());
        aQueue.pop();
        const uint32_t cTarget = aTargetOf(effect);
        const bool cTargetWaiting = std::find(waiting.begin(), waiting.end(), cTarget) != waiting.end();
        if (!aTake(effect, cTargetWaiting))
            continue;
        if (!cTargetWaiting)
            waiting.push_back(cTarget);
        aQueue.push(std::move(effect));
    }
}
} // namespace EffectQueue
