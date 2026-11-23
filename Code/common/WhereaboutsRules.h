#pragma once

#include <cstdint>

// How often the server tells everyone where everyone is (NotifyPlayerWhereabouts), and how a world map icon follows.
namespace WhereaboutsRules
{
//! While a position changes, it is told this often (ms): the map icon of a running player then moves a few units at a time.
inline constexpr uint64_t kMovingEveryMs = 250;
//! Even a still player is told this often (ms): a game that loaded late, or emptied its map's list in a load, hears again.
inline constexpr uint64_t kHeartbeatMs = 2000;

//! Whether the next message is due, aSinceLastMs after the last one; aChanged: what it would say differs from the last.
inline bool SendDue(const uint64_t aSinceLastMs, const bool aChanged) noexcept
{
    return aSinceLastMs >= kHeartbeatMs || (aChanged && aSinceLastMs >= kMovingEveryMs);
}

//! A map marker moves only once its player has moved this far (about 3.5 m)...
inline constexpr float kFollowDistance = 256.f;
//! ... or this far while the world map is open, where the icon is seen to move (a little over 20 cm).
inline constexpr float kFollowDistanceMapOpen = 16.f;

//! The distance a marker waits for before it moves.
inline float FollowDistance(const bool aMapOpen) noexcept
{
    return aMapOpen ? kFollowDistanceMapOpen : kFollowDistance;
}
} // namespace WhereaboutsRules
