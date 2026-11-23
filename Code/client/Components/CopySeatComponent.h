#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <NpcFurniture.h>
#include <Structs/GameId.h>

//! On a copy of an NPC another game runs: the furniture it sits or sleeps in at its owner, as the owner last said
//! (FurnitureService), and whether this game put the copy there.
struct CopySeatComponent
{
    NpcFurniture::Seat Where{};
    //! When its owner last said so (FurnitureService's clock, in ms).
    uint64_t AtMs{};
    //! The furniture this game put the copy in, by form id here; 0 when it put it in none.
    uint32_t SeatedInFormId{};
    //! The state this game seated it in (sitting or sleeping), as the game read it back; 0 when it seated none.
    uint8_t SeatedState{};
    //! When this game last tried to seat it and the game refused; 0 when it has not been refused.
    uint64_t RefusedAtMs{};
    //! When its owner first said its NPC got up while this game had the copy seated; 0 while it is seated at its owner.
    uint64_t StandAskedAtMs{};
    //! The NPC's server id, and the epoch of the owner the word came from.
    uint32_t ServerId{};
    uint32_t OwnershipEpoch{};
    //! Nonzero for a player's server-granted seat.
    uint64_t Lease{};
    GameId FurnitureCell{};
    //! The decision the copy's queued seat belongs to (CopyWork::Tickets).
    uint32_t Ticket{};
    //! The copy this game seated, by handle (the game reuses a freed form id); 0 when it seated none.
    uint32_t CopyHandle{};
    //! The marker this game last asked for, and the one the game gave.
    uint32_t AskedMarker{};
    uint32_t GivenMarker{};
    //! When a seat or stand was handed to the main thread and has not come back yet; 0 when none is out.
    uint64_t WorkOutAtMs{};
    //! The last seat or stand handed out, so that only its receipt is taken.
    NpcFurniture::WorkId Work{};
    //! The decision whose seat this game put in place (CopyWork::Tickets::InPlace); a stand undoes that one only. 0 for none.
    uint32_t PlacedTicket{};
    //! For a seat placed at run time (Where.Placement): the furniture found here for it, by form id, 0 for none; and when it was
    //! last looked for, as the search walks the loaded cells' references.
    uint32_t RuntimeFormId{};
    uint64_t RuntimeLookedAtMs{};
};
