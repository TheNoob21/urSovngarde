#pragma once

#include <ObjectMover.h>
#include <Structs/GameId.h>

struct PropBodyRelay
{
    using Clock = ObjectMover::Clock;
    static constexpr size_t kMaxActive = 256;
    static constexpr uint32_t kUpdatesPerSecond = 128;

    struct Rate
    {
        Clock::time_point Since{};
        uint32_t Count{};

        bool Allow(Clock::time_point aNow, uint32_t aPerSecond = kUpdatesPerSecond) noexcept
        {
            if (aNow - Since >= std::chrono::seconds(1))
            {
                Since = aNow;
                Count = 0;
            }
            if (Count >= aPerSecond)
                return false;
            ++Count;
            return true;
        }
    };

    ObjectMover Mover;
    GameId Cell{};
    GameId WorldSpace{};
    Clock::time_point Last{};
    uint64_t Tick{};
    bool AtRest{};

    bool Accept(uint32_t aPlayer, const GameId& acCell, const GameId& acWorldSpace, bool aAtRest, uint64_t aTick, Clock::time_point aNow, Rate& aRate) noexcept
    {
        const bool cActive = !Mover.IsLapsed(aNow);
        if (cActive && (Mover.GetPlayerId() != aPlayer || Cell != acCell || WorldSpace != acWorldSpace))
            return false;
        // Swings travel unreliable: one the same player sent before the last passed on came late, even after a rest.
        if (aTick && Tick && aTick <= Tick && Mover.GetPlayerId() == aPlayer)
            return false;
        if (aAtRest)
        {
            if (!cActive || AtRest)
                return false;
        }
        else if ((cActive && aNow - Last < std::chrono::milliseconds(60)) || !aRate.Allow(aNow))
            return false;

        Mover.Take(aPlayer, aAtRest, aNow);
        Cell = acCell;
        WorldSpace = acWorldSpace;
        Last = aNow;
        Tick = aTick;
        AtRest = aAtRest;
        return true;
    }
};
