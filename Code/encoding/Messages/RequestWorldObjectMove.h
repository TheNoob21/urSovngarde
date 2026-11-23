#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/Vector3_NetQuantize.h>

//! A placed world object (a bottle, a cart) is held, thrown or pushed here; this is where it is now.
struct RequestWorldObjectMove final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestWorldObjectMove;

    RequestWorldObjectMove()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestWorldObjectMove& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ObjectId == acRhs.ObjectId && CellId == acRhs.CellId && WorldSpaceId == acRhs.WorldSpaceId && Position == acRhs.Position &&
               Rotation == acRhs.Rotation && AtRest == acRhs.AtRest && Taken == acRhs.Taken && Tick == acRhs.Tick && Thrown == acRhs.Thrown && Velocity == acRhs.Velocity &&
               Spin == acRhs.Spin;
    }

    //! The object's own reference id, the same in every game that has the plugin it comes from.
    GameId ObjectId{};
    GameId CellId{};
    GameId WorldSpaceId{};
    Vector3_NetQuantize Position{};
    glm::vec3 Rotation{};
    //! The last of a movement: the object has stopped, and the receiver hands it back to its own physics.
    bool AtRest{};
    //! Taken into a player's inventory: the receiver disables its own copy of the reference.
    //! Any placed item but a quest object (PlacedTakes); the server keeps it taken for a player who comes later.
    bool Taken{};
    //! When the sender sampled the place, on the clock every movement report uses (0: not given), so the receiver shows the
    //! move's speed and arc as the sender had it, whatever the network did to the packets.
    uint64_t Tick{};
    //! A hand let go of it and it flies on by itself from here: the receiver's own physics flies it, at Velocity and Spin.
    bool Thrown{};
    //! Its speed (game units a second) and spin (radians a second, about the world's axes) when Thrown.
    glm::vec3 Velocity{};
    glm::vec3 Spin{};
};
