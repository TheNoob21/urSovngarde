#pragma once

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

//! Whether a player may move a placed world object or a dropped item for everybody: only one near it.
//! A move is kept by the server and replayed to every player who comes near later, so a player far away must not send one.
//! Pure rules on plain values, so they can be tested (tests/mover_nearness.cpp).
namespace MoverNearness
{
//! A placed world object is watched only within 800 units of a player, but a thrown or shouted one can fly far from its
//! thrower while its moves are still sent, so this is a cell's width.
constexpr float kMaxDistance = 4096.f;
//! A dropped item moves by physics anywhere in a game's loaded grid (5 by 5 cells of 4096 units, the player in the middle one),
//! and that game sends it: as far as the grid's far corner.
constexpr float kLoadedGridReach = 16384.f;

//! aCellInRange: the sender's loaded grid holds the object's cell, in the same worldspace (or the same interior).
//! aHasPosition: the sender's character has sent a position; aSender, aObject: where the character and the object are.
//! aMaxDistance: kMaxDistance for a placed world object, kLoadedGridReach for a dropped item.
inline bool IsNear(const bool aCellInRange, const bool aHasPosition, const glm::vec3& acSender, const glm::vec3& acObject, const float aMaxDistance = kMaxDistance) noexcept
{
    return aCellInRange && aHasPosition && glm::distance(acSender, acObject) <= aMaxDistance;
}
} // namespace MoverNearness
