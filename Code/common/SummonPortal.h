#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

//! The portal another player's summon comes out of.
//!
//! The summon's cast is never replayed here (it would make a second creature); the creature comes as a copy of its owner's.
//! So the portal its effect plays where the creature appears (its hit art, SummonTargetFX in Skyrim.esm) was not seen. The
//! cast still arrives, so its portal art is kept for a few seconds, and the next copy of a player's summon that appears
//! here in that time plays it. A summon a player only comes near, long after its cast, plays none.
namespace SummonPortal
{
//! How long after the cast a summon's copy still plays its portal (ms): the creature is made, sent, spawned and loaded here.
inline constexpr uint64_t kWindowMs = 5000;
//! How long the portal is played on the copy (s).
inline constexpr float kPortalSeconds = 3.f;
//! Summons cast at once by everyone near; the oldest is forgotten first.
inline constexpr size_t kKept = 4;

//! Arts are named by an opaque value of the game's (its form, which lives as long as the game); 0 for none.
struct Pending
{
    uint64_t ArtId{};
    uint64_t At{};
};

struct Queue
{
    std::array<Pending, kKept> Items{};
    size_t Count{};

    //! A summon was cast with portal art aArtId (0: none to show) at aNowMs.
    void Note(const uint64_t aArtId, const uint64_t aNowMs) noexcept
    {
        if (aArtId == 0)
            return;
        if (Count == kKept)
        {
            for (size_t i = 1; i < Count; ++i)
                Items[i - 1] = Items[i];
            --Count;
        }
        Items[Count++] = {aArtId, aNowMs};
    }

    //! The portal art for a summon's copy appearing at aNowMs (0: none): the oldest cast still in its window, used up.
    uint64_t Take(const uint64_t aNowMs) noexcept
    {
        size_t kept = 0;
        uint64_t taken = 0;
        for (size_t i = 0; i < Count; ++i)
        {
            const Pending cItem = Items[i];
            const bool cFresh = aNowMs >= cItem.At && aNowMs - cItem.At <= kWindowMs;
            if (!cFresh)
                continue;
            if (taken == 0)
            {
                taken = cItem.ArtId;
                continue;
            }
            Items[kept++] = cItem;
        }
        Count = kept;
        return taken;
    }
};
} // namespace SummonPortal
