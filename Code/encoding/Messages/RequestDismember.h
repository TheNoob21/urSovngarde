#pragma once

#include "Message.h"

//! Parts of an actor this game runs that the game cut off (its head, in the engine's limb bits), for the other games to cut off too.
//! The server adds the limbs to what it keeps for the actor, so a game that loads it later has them cut off, and relays it as NotifyDismember.
struct RequestDismember final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestDismember;

    RequestDismember()
        : ClientMessage(Opcode)
    {
    }

    virtual ~RequestDismember() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestDismember& acRhs) const noexcept
    {
        return ServerId == acRhs.ServerId && Limbs == acRhs.Limbs && LastHitLimb == acRhs.LastHitLimb && GetOpcode() == acRhs.GetOpcode();
    }

    uint32_t ServerId{};
    //! One bit per limb cut off, as the engine numbers them.
    uint16_t Limbs{};
    //! The limb that took the last blow.
    uint8_t LastHitLimb{};
};
