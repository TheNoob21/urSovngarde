#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

//! On a copy of an NPC another game runs: whom it fights at its owner, as the owner last said (CombatService).
//! Server ids only: the target is looked up again each time it is used.
struct CopyFightComponent
{
    //! Whom it fights at its owner.
    uint32_t TargetServerId{};
    //! When its owner last said so (GetTickCount64).
    uint64_t AtMs{};
    //! Whom the copy's head follows here now, 0 for nobody: set by the script look-at, which lasts until it is cleared.
    uint32_t LookingAtServerId{};
    //! The target last found missing here, so that it is said once.
    uint32_t MissingServerId{};
    //! The target the copy's own combat controller was last pointed at, so that it is said once.
    uint32_t PointedServerId{};
};
