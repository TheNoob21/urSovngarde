#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <NpcFurniture.h>

#include <chrono>

//! On an NPC: the furniture its owner's game last said it sits or sleeps in, and when that last went to the other games
//! (FurnitureService). A seat that does not change goes on again at most every NpcFurniture::kRelayEveryMs.
struct SeatComponent
{
    NpcFurniture::Seat Relayed{};
    uint32_t RelayedEpoch{};
    std::chrono::steady_clock::time_point RelayedAt{};
};
