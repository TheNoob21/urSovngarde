#pragma once

#include "Message.h"

#include <Structs/GameId.h>

//! An arrow stuck in a wall or the ground that this game's player took (its projectile, its cell and where it was), so the
//! other games remove their own copy of it (ArrowPickup). The server checks it and relays it as NotifyArrowPickup; nothing is kept.
struct RequestArrowPickup final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestArrowPickup;

    RequestArrowPickup()
        : ClientMessage(Opcode)
    {
    }

    virtual ~RequestArrowPickup() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestArrowPickup& acRhs) const noexcept
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
