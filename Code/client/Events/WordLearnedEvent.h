#pragma once

/**
 * @brief A script taught this player a word of power (Game.TeachWord): a word wall, or a quest.
 */
struct WordLearnedEvent
{
    explicit WordLearnedEvent(uint32_t aWordId)
        : WordId(aWordId)
    {
    }

    uint32_t WordId;
};
