#pragma once

/**
 * @brief This game's player activated an arrow stuck in a wall or the ground (a temporary projectile reference), and the game
 * accepted it. ArrowPickupService sends it once it sees the arrow really gone from the world.
 */
struct ArrowPickupEvent
{
    //! Ids and a place, not pointers: the reference is taken away by the time the event is handled.
    uint32_t RefId{};
    uint32_t ProjectileBaseId{};
    uint32_t CellId{};
    float X{};
    float Y{};
    float Z{};
};
