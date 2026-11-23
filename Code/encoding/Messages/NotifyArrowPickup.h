#pragma once

#include "Message.h"

#include <Structs/GameId.h>

//! An arrow another player took out of a wall or the ground (RequestArrowPickup, checked by the server): this game removes its
//! own copy of the same projectile nearest that place, if one is close enough (ArrowPickup).
struct NotifyArrowPickup final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyArrowPickup;

    NotifyArrowPickup()
        : ServerMessage(Opcode)
    {
    }

    virtual ~NotifyArrowPickup() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyArrowPickup& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ProjectileBaseId == acRhs.ProjectileBaseId && CellId == acRhs.CellId && X == acRhs.X && Y == acRhs.Y && Z == acRhs.Z;
    }

    //! The arrow's projectile (the stuck arrow's base form).
    GameId ProjectileBaseId{};
    //! The cell the arrow was in.
    GameId CellId{};
    //! Where the arrow was.
    float X{};
    float Y{};
    float Z{};
};
