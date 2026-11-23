#include <catch2/catch.hpp>

#include <Systems/RemoteBodySlots.h>

#include <array>
#include <cstdint>

using RemoteBodySlots::Choice;
using RemoteBodySlots::Claim;
using RemoteBodySlots::Slot;

namespace
{
BodyPoseCommit::RemotePlayerKey Player(const uint32_t aServer)
{
    return {7, aServer};
}
// What the registry does with a choice: a new copy takes the slot over, bound.
template <size_t N> void Apply(std::array<Slot, N>& aSlots, const Choice& acChoice, const uint32_t aActor, const BodyPoseCommit::RemotePlayerKey& acPlayer, const double aNow)
{
    if (acChoice.Kind == Claim::None)
        return;
    auto& slot = aSlots[acChoice.Index];
    if (acChoice.Kind != Claim::Held)
        slot = {aActor, acPlayer};
    RemoteBodySlots::Observe(slot, true, aNow);
}
} // namespace

TEST_CASE("Each remote VR player's copy gets its own slot and keeps it", "[remote_body_slots]")
{
    std::array<Slot, 3> slots{};
    const std::array<uint32_t, 3> cCopies{0xFF000801u, 0xFF000802u, 0xFF000803u};
    for (size_t i = 0; i < cCopies.size(); ++i)
    {
        const auto cChoice = RemoteBodySlots::Choose(slots, cCopies[i], Player(10 + static_cast<uint32_t>(i)), 1000.0);
        CHECK(cChoice.Kind == Claim::Free);
        CHECK(cChoice.Index == i);
        Apply(slots, cChoice, cCopies[i], Player(10 + static_cast<uint32_t>(i)), 1000.0);
    }
    // The same copies later: each finds its own slot, whatever order they publish in.
    for (size_t i = cCopies.size(); i-- > 0;)
    {
        const auto cChoice = RemoteBodySlots::Choose(slots, cCopies[i], Player(10 + static_cast<uint32_t>(i)), 1100.0);
        CHECK(cChoice.Kind == Claim::Held);
        CHECK(cChoice.Index == i);
    }
    // No FormID, no slot.
    CHECK(RemoteBodySlots::Choose(slots, 0, Player(20), 1100.0).Kind == Claim::None);
}

TEST_CASE("A copy beyond the slot bound stays on the frame-end poser until a slot is free", "[remote_body_slots]")
{
    std::array<Slot, 2> slots{};
    Apply(slots, RemoteBodySlots::Choose(slots, 0xFF000801u, Player(10), 1000.0), 0xFF000801u, Player(10), 1000.0);
    Apply(slots, RemoteBodySlots::Choose(slots, 0xFF000802u, Player(11), 1000.0), 0xFF000802u, Player(11), 1000.0);
    CHECK(RemoteBodySlots::Choose(slots, 0xFF000803u, Player(12), 1000.0).Kind == Claim::None);

    // The second copy lost its binding: its slot is held for the hold time, then another player's copy may take it.
    RemoteBodySlots::Observe(slots[0], true, 1500.0);
    RemoteBodySlots::Observe(slots[1], false, 1500.0);
    CHECK(slots[1].UnboundSinceMs == 1500.0);
    RemoteBodySlots::Observe(slots[1], false, 1600.0); // a later observation keeps the first time it was seen unbound
    CHECK(slots[1].UnboundSinceMs == 1500.0);
    CHECK(RemoteBodySlots::Choose(slots, 0xFF000803u, Player(12), 1500.0 + BodyPoseCommit::kSelectedReselectHoldMs - 1.0).Kind == Claim::None);
    const auto cChoice = RemoteBodySlots::Choose(slots, 0xFF000803u, Player(12), 1500.0 + BodyPoseCommit::kSelectedReselectHoldMs);
    CHECK(cChoice.Kind == Claim::Reclaim);
    CHECK(cChoice.Index == 1);

    // Bound again within the hold, the slot stays with its copy.
    RemoteBodySlots::Observe(slots[1], true, 2000.0);
    CHECK(slots[1].UnboundSinceMs == 0.0);
    CHECK(RemoteBodySlots::Choose(slots, 0xFF000803u, Player(12), 1500.0 + BodyPoseCommit::kSelectedReselectHoldMs).Kind == Claim::None);

    // A freed slot (the copy left) goes to the waiting copy at once.
    slots[0] = {};
    const auto cFree = RemoteBodySlots::Choose(slots, 0xFF000803u, Player(12), 2000.0);
    CHECK(cFree.Kind == Claim::Free);
    CHECK(cFree.Index == 0);
}

TEST_CASE("A respawned copy of a remote player takes over that player's slot once the old copy is unbound", "[remote_body_slots]")
{
    std::array<Slot, 3> slots{};
    Apply(slots, RemoteBodySlots::Choose(slots, 0xFF0008E1u, Player(10), 1000.0), 0xFF0008E1u, Player(10), 1000.0);
    Apply(slots, RemoteBodySlots::Choose(slots, 0xFF0008E2u, Player(11), 1000.0), 0xFF0008E2u, Player(11), 1000.0);

    // The old copy is still bound: the new one waits on the frame-end poser and takes no free slot, so one player never
    // holds two slots.
    CHECK(RemoteBodySlots::Choose(slots, 0xFF000946u, Player(10), 1100.0).Kind == Claim::None);

    // Unbound: the new copy takes the same slot, no hold needed.
    RemoteBodySlots::Observe(slots[0], false, 1200.0);
    const auto cChoice = RemoteBodySlots::Choose(slots, 0xFF000946u, Player(10), 1200.0);
    CHECK(cChoice.Kind == Claim::Rebind);
    CHECK(cChoice.Index == 0);

    // A copy whose player is unknown (no pose session yet) never rebinds; it takes a free slot.
    const auto cUnknown = RemoteBodySlots::Choose(slots, 0xFF000950u, BodyPoseCommit::RemotePlayerKey{}, 1200.0);
    CHECK(cUnknown.Kind == Claim::Free);
    CHECK(cUnknown.Index == 2);
}

TEST_CASE("An empty slot is never seen as bound or waiting", "[remote_body_slots]")
{
    Slot slot{};
    RemoteBodySlots::Observe(slot, true, 1000.0);
    CHECK_FALSE(slot.Live);
    CHECK(slot.UnboundSinceMs == 0.0);
}
