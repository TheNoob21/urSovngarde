#pragma once

#include <Structs/GameId.h>

#include <unordered_map>

//! The weather a party's members last reported, one for each worldspace.
//! One value for the whole party let the last reporter win: a member entering another region got the weather of the first.
//! Indoors there is no weather to keep.
class PartyWeather
{
public:
    void Set(const GameId& acWorldSpaceId, const GameId& acWeatherId) noexcept
    {
        if (acWorldSpaceId)
            m_weathers[acWorldSpaceId] = acWeatherId;
    }

    //! None when nobody reported one there yet.
    [[nodiscard]] GameId Get(const GameId& acWorldSpaceId) const noexcept
    {
        const auto it = m_weathers.find(acWorldSpaceId);
        return it != m_weathers.end() ? it->second : GameId{};
    }

private:
    std::unordered_map<GameId, GameId> m_weathers;
};
