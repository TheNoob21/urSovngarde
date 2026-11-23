#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <NpcHeadTrack.h>

#include <chrono>

//! On an NPC: where its owner's game last said it looks, and when that last went to the other games (HeadTrackService).
//! A look that does not change goes on again at most every NpcHeadTrack::kRelayEveryMs.
struct HeadTrackComponent
{
    NpcHeadTrack::Look Relayed{};
    uint32_t RelayedEpoch{};
    std::chrono::steady_clock::time_point RelayedAt{};
};
