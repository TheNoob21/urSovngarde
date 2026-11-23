#pragma once

/**
 * @brief A bear trap's script played the animation that springs or arms it here (ReferenceStates::BearTraps).
 */
struct BearTrapEvent
{
    BearTrapEvent(uint32_t aFormId, bool aArmed, bool aLoad)
        : FormId(aFormId)
        , Armed(aArmed)
        , Load(aLoad)
    {
    }

    //! An id, not a pointer: the event is handled on a later update.
    uint32_t FormId;
    bool Armed;
    //! The trap's load armed it (StartOpen), not a step or a use.
    bool Load;
};
