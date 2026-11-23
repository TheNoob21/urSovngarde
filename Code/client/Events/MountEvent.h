#pragma once

/**
 * @brief A local actor's native mount package was accepted (horse or dragon).
 * This is intent, not confirmation that the actor reached a seat.
 */
struct MountEvent
{
    MountEvent(uint32_t aRiderID, uint32_t aMountID)
        : RiderID(aRiderID)
        , MountID(aMountID)
    {
    }

    uint32_t RiderID;
    uint32_t MountID;
};
