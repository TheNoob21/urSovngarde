#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <NpcHeadTrack.h>

//! On a copy of an NPC another game runs: where its owner's NPC looks, as the owner last said (HeadTrackService),
//! and the point this game shows the copy looking at now.
struct CopyHeadTrackComponent
{
    NpcHeadTrack::Look Wanted{};
    //! When its owner last said so (HeadTrackService's clock, in ms).
    uint64_t AtMs{};
    //! The owner's epoch the word was taken at: a copy whose epoch moved on since is another owner's, and this word is dropped.
    uint32_t OwnershipEpoch{};
    //! The form the look was first written into: a copy bound to another form since is not written to.
    uint32_t FormId{};
    //! The point written into the copy's graph last frame, eased towards Wanted.
    NpcHeadTrack::Point Shown{};
    //! Whether Shown holds a point: false until the first look is written, and again after the combat look held the copy.
    bool Showing{false};
    //! The flags written last frame, and the time since the last write (ms): an unchanged look is only written again at intervals.
    uint8_t ShownFlags{};
    float PendingMs{};
    //! Whether this game wrote a look into the copy's graph since it last told it to stop.
    bool Written{false};
};
