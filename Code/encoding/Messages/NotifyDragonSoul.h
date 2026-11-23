#pragma once

#include "Message.h"

// A dragon died: its soul goes to every player of its owner's party, once per dragon (DragonSoul.h).
struct NotifyDragonSoul final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyDragonSoul;

    NotifyDragonSoul()
        : ServerMessage(Opcode)
    {
    }

    virtual ~NotifyDragonSoul() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyDragonSoul& acRhs) const noexcept { return ServerId == acRhs.ServerId && GetOpcode() == acRhs.GetOpcode(); }

    uint32_t ServerId{};
};
