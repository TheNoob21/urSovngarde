#pragma once

#include "Message.h"

//! A hit made an NPC stagger or recoil in the hitter's game; its owner's game plays it (HitReactionService).
struct NotifyHitReaction final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyHitReaction;

    NotifyHitReaction()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyHitReaction& acRhs) const noexcept
    {
        return Id == acRhs.Id && Kind == acRhs.Kind && Magnitude == acRhs.Magnitude && Direction == acRhs.Direction && GetOpcode() == acRhs.GetOpcode();
    }

    uint32_t Id{};
    //! HitReactions::Kind: 0 stagger, 1 recoil, 2 large recoil.
    uint8_t Kind{};
    //! The graph's staggerMagnitude, 0 to 1.
    float Magnitude{};
    //! The graph's staggerDirection: where the blow came from, a fraction of a turn from where the actor faces.
    float Direction{};
};
