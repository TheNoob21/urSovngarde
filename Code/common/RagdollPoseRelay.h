#pragma once

#include <cstdint>

// Whether the server passes on where the bodies of a moved creature's ragdoll are (RagdollPoseService), with no game in it.
// Only the game that runs the actor says it, never for a player's character. Its game sends about ten a second; a network may
// deliver a late one together with the next, so two may come close together, but not a third.
namespace RagdollPoseRelay
{
// No third pose of an actor is passed on within this long of the one before last.
constexpr uint64_t kBurstMs = 90;

// When the last two poses of an actor were passed on, in milliseconds; 0 for none.
struct Relayed
{
    uint64_t Last{};
    uint64_t BeforeLast{};
};

[[nodiscard]] constexpr bool
ServerTakes(const bool aExists, const bool aFromOwner, const bool aIsPlayer, const bool aHasBodies, const Relayed& acRelayed, const uint64_t aNowMs) noexcept
{
    if (!aExists || !aFromOwner || aIsPlayer || !aHasBodies)
        return false;
    return acRelayed.BeforeLast == 0 || aNowMs < acRelayed.BeforeLast || aNowMs - acRelayed.BeforeLast >= kBurstMs;
}

[[nodiscard]] constexpr Relayed AfterRelay(const Relayed& acRelayed, const uint64_t aNowMs) noexcept
{
    return Relayed{aNowMs, acRelayed.Last};
}
} // namespace RagdollPoseRelay
