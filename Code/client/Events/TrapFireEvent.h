#pragma once

/**
 * @brief A trap trigger (TrapSync::kTriggerBases) activated a reference here: it fired its trap.
 */
struct TrapFireEvent
{
    TrapFireEvent(uint32_t aTrapId, uint32_t aTriggerId)
        : TrapId(aTrapId)
        , TriggerId(aTriggerId)
    {
    }

    //! Ids, not pointers: the event is handled on a later update.
    uint32_t TrapId;
    uint32_t TriggerId;
};
