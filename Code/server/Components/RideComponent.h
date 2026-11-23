#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <MountSync.h>

//! On a rider: the horse its owner's game last said it rides (CharacterService::OnMountRequest), so that a player sent the
//! pair later is told at once instead of at the next refresh.
struct RideComponent
{
    MountSync::Ride Ride{};
};
