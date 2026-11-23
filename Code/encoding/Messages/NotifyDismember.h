#pragma once

#include "Message.h"

//! Parts of an actor another game cut off, relayed by the server from RequestDismember.
struct NotifyDismember final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyDismember;

    NotifyDismember()
        : ServerMessage(Opcode)
    {
    }

    virtual ~NotifyDismember() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyDismember& acRhs) const noexcept
    {
        return ServerId == acRhs.ServerId && Limbs == acRhs.Limbs && LastHitLimb == acRhs.LastHitLimb && GetOpcode() == acRhs.GetOpcode();
    }

    uint32_t ServerId{};
    //! One bit per limb cut off, as the engine numbers them.
    uint16_t Limbs{};
    //! The limb that took the last blow.
    uint8_t LastHitLimb{};
};
