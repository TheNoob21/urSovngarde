#pragma once

// The animation steps of copies whose AI is skipped, run between frames: plain values, so the rule is unit-tested.
//
// A copy's graph update takes the graph's lock and then reads the physics world. Run from the actor process jobs it can
// meet a physics step that holds the world and reads that graph (PLANCK through HIGGS), and the two wait for each other.
// Between frames no step holds the world.

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

class CopyAnimationQueue
{
public:
    //! A copy's step never covers more than this: a copy that missed frames (a load, a menu) is not jumped far ahead.
    static constexpr float kMaxStep = 0.1f;

    // Called from the actor process jobs, any thread.
    void Add(uint32_t aFormId, float aDeltaTime)
    {
        if (!aFormId || !(aDeltaTime > 0.f))
            return;
        std::lock_guard lock(m_mutex);
        float& step = m_steps[aFormId];
        step = std::min(step + aDeltaTime, kMaxStep);
    }

    // The steps queued since the last call, each copy once.
    [[nodiscard]] std::vector<std::pair<uint32_t, float>> Take()
    {
        std::unordered_map<uint32_t, float> steps;
        {
            std::lock_guard lock(m_mutex);
            steps.swap(m_steps);
        }
        return {steps.begin(), steps.end()};
    }

private:
    std::mutex m_mutex;
    std::unordered_map<uint32_t, float> m_steps;
};
