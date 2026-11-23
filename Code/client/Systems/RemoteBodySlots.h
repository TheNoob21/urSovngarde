#pragma once

#include <Systems/BodyPoseCommit.h>

#include <array>
#include <cstddef>
#include <cstdint>

// The remote VR players' copies SEBodyPresentation commits natively on a flat screen: one slot per copy, at most N at
// once. A copy keeps its slot while it has one; a copy with none is posed at frame end (RemoteBodyPoser).
namespace RemoteBodySlots
{
struct Slot
{
    uint32_t Actor{};
    BodyPoseCommit::RemotePlayerKey Player{};
    bool Live{};             // the copy has a binding
    double UnboundSinceMs{}; // when it was first seen without one (0 = bound, or not seen unbound yet)
};

enum class Claim : uint8_t
{
    None,    // no slot: frame end poses the copy
    Held,    // the copy's own slot
    Rebind,  // another copy of the slot's remote player, whose copy has no binding left
    Free,    // an empty slot
    Reclaim, // every slot taken: one whose copy has had no binding for BodyPoseCommit::kSelectedReselectHoldMs
};

struct Choice
{
    Claim Kind = Claim::None;
    size_t Index{};
};

// Live is the copy's binding now; the hold starts the first time the copy is seen without one.
inline void Observe(Slot& aSlot, const bool aLive, const double aNowMs) noexcept
{
    aSlot.Live = aSlot.Actor && aLive;
    if (aSlot.Live || !aSlot.Actor)
        aSlot.UnboundSinceMs = 0.0;
    else if (!(aSlot.UnboundSinceMs > 0.0))
        aSlot.UnboundSinceMs = aNowMs;
}

// The slot aActor's copy holds or may take. Two copies of one remote player never hold slots at once.
template <size_t N>
[[nodiscard]] Choice Choose(const std::array<Slot, N>& acSlots, const uint32_t aActor, const BodyPoseCommit::RemotePlayerKey& acPlayer, const double aNowMs) noexcept
{
    if (!aActor)
        return {};
    for (size_t i = 0; i < N; ++i)
        if (acSlots[i].Actor == aActor)
            return {Claim::Held, i};
    for (size_t i = 0; i < N; ++i)
    {
        const auto& cSlot = acSlots[i];
        if (cSlot.Actor && BodyPoseCommit::SameRemotePlayer(cSlot.Player, acPlayer))
        {
            if (BodyPoseCommit::DecideSelectedRebind(cSlot.Actor, aActor, cSlot.Player, acPlayer, cSlot.Live) == BodyPoseCommit::SelectedRebind::Rebind)
                return {Claim::Rebind, i};
            return {};
        }
    }
    for (size_t i = 0; i < N; ++i)
        if (!acSlots[i].Actor)
            return {Claim::Free, i};
    for (size_t i = 0; i < N; ++i)
    {
        const auto& cSlot = acSlots[i];
        if (BodyPoseCommit::DecideSelectedReselect(cSlot.Actor, aActor, cSlot.Live, cSlot.UnboundSinceMs, aNowMs) == BodyPoseCommit::SelectedReselect::Reselect)
            return {Claim::Reclaim, i};
    }
    return {};
}
} // namespace RemoteBodySlots
