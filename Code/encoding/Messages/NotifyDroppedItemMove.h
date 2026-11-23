#pragma once

#include "Message.h"

#include <Structs/Vector3_NetQuantize.h>

//! A remembered item moved on another player's side: put it where they have it.
struct NotifyDroppedItemMove final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyDroppedItemMove;

    NotifyDroppedItemMove()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyDroppedItemMove& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Id == acRhs.Id && Position == acRhs.Position && Rotation == acRhs.Rotation && AtRest == acRhs.AtRest && Tick == acRhs.Tick &&
               MoverPlayerId == acRhs.MoverPlayerId && Thrown == acRhs.Thrown && Velocity == acRhs.Velocity && Spin == acRhs.Spin && Fixed == acRhs.Fixed;
    }

    //! The server's id for the item, as NotifyDroppedItem gave it.
    uint32_t Id{};
    Vector3_NetQuantize Position{};
    glm::vec3 Rotation{};
    //! The item has stopped: the server keeps this place, and the receiver hands it back to physics.
    bool AtRest{};
    //! When the sender sampled the place, on the clock every movement report uses (0: not given), so the receiver shows the
    //! move's speed and arc as the sender had it, whatever the network did to the packets.
    uint64_t Tick{};
    //! The player moving it (0: not given), so the receiver draws it from that player's hand and not the nearest one.
    uint32_t MoverPlayerId{};
    //! A hand let go of it and it flies on by itself from here: the receiver's own physics flies it, at Velocity and Spin.
    bool Thrown{};
    //! Its speed (game units a second) and spin (radians a second, about the world's axes) when Thrown.
    glm::vec3 Velocity{};
    glm::vec3 Spin{};
    //! Put down and held still there by a script (a weapon rack's weapon): kept where it is, not handed to physics.
    //! Only with AtRest; appended last.
    bool Fixed{};
};
