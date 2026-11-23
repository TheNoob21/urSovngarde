#include <catch2/catch.hpp>

#include <EffectQueue.h>

#include <queue>
#include <vector>

namespace
{
struct Effect
{
    uint32_t Id{};
    uint32_t Target{};
    bool Ready{};
    bool Expired{};
};

// One round as the magic service takes it: an expired effect is dropped, one on a waiting target or not ready yet is kept.
std::vector<uint32_t> TakeRound(std::queue<Effect>& aQueue)
{
    std::vector<uint32_t> applied;
    EffectQueue::Round(
        aQueue, [](const Effect& acEffect) { return acEffect.Target; },
        [&applied](const Effect& acEffect, const bool aTargetWaiting)
        {
            if (acEffect.Expired)
                return false;
            if (aTargetWaiting || !acEffect.Ready)
                return true;
            applied.push_back(acEffect.Id);
            return false;
        });
    return applied;
}

std::vector<uint32_t> IdsOf(std::queue<Effect> aQueue)
{
    std::vector<uint32_t> ids;
    for (; !aQueue.empty(); aQueue.pop())
        ids.push_back(aQueue.front().Id);
    return ids;
}
} // namespace

TEST_CASE("An effect waiting for its target holds back no other target's effects", "[effect_queue]")
{
    std::queue<Effect> queue;
    queue.push({1, 10, false, false});
    queue.push({2, 20, true, false});
    queue.push({3, 30, true, false});

    CHECK(TakeRound(queue) == std::vector<uint32_t>{2, 3});
    CHECK(IdsOf(queue) == std::vector<uint32_t>{1});
}

TEST_CASE("A target takes its held-back effects in order", "[effect_queue]")
{
    std::queue<Effect> queue;
    queue.push({1, 10, false, false});
    queue.push({2, 20, true, false});
    // Ready, but an earlier effect on target 10 still waits.
    queue.push({3, 10, true, false});

    CHECK(TakeRound(queue) == std::vector<uint32_t>{2});
    CHECK(IdsOf(queue) == std::vector<uint32_t>{1, 3});

    // Target 10 is known now: both go, the first first.
    std::queue<Effect> next;
    for (; !queue.empty(); queue.pop())
    {
        Effect effect = queue.front();
        effect.Ready = true;
        next.push(effect);
    }
    CHECK(TakeRound(next) == std::vector<uint32_t>{1, 3});
    CHECK(next.empty());
}

TEST_CASE("An expired effect is dropped and does not hold back its target", "[effect_queue]")
{
    std::queue<Effect> queue;
    queue.push({1, 10, false, true});
    queue.push({2, 10, true, false});

    CHECK(TakeRound(queue) == std::vector<uint32_t>{2});
    CHECK(queue.empty());
}

TEST_CASE("An effect queued during a round waits for the next one", "[effect_queue]")
{
    std::queue<Effect> queue;
    queue.push({1, 10, true, false});

    size_t looked = 0;
    EffectQueue::Round(
        queue, [](const Effect& acEffect) { return acEffect.Target; },
        [&](const Effect&, bool)
        {
            ++looked;
            queue.push({2, 20, true, false});
            return false;
        });

    CHECK(looked == 1);
    CHECK(IdsOf(queue) == std::vector<uint32_t>{2});
}
