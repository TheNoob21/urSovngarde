#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

//! Which other player's copy carries an object that player moves.
namespace HeldMover
{
//! The copy of the player the server named as the mover (each copy has a PlayerId), when that copy is here. None when no
//! player was named or its copy is not here: the receiver then goes by the nearest copy and hand, as before the server named one.
//! With two players side by side, the nearest hand could be the other player's.
template <class T> std::optional<size_t> Named(const T& acCopies, const uint32_t aMoverPlayerId) noexcept
{
    if (aMoverPlayerId == 0)
        return std::nullopt;
    for (size_t i = 0; i < acCopies.size(); ++i)
    {
        if (acCopies[i].PlayerId == aMoverPlayerId)
            return i;
    }
    return std::nullopt;
}
} // namespace HeldMover
