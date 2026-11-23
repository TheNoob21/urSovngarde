#pragma once

#include "Message.h"

struct NotifyMount final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyMount;

    NotifyMount()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyMount& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && RiderId == acRhs.RiderId && MountId == acRhs.MountId && RiderOwnershipEpoch == acRhs.RiderOwnershipEpoch &&
               MountOwnershipEpoch == acRhs.MountOwnershipEpoch && IsRefresh == acRhs.IsRefresh;
    }

    uint32_t RiderId{};
    uint32_t MountId{};
    // Post-transfer epochs: a delayed package must not act on a different authority's copies.
    uint32_t RiderOwnershipEpoch{};
    uint32_t MountOwnershipEpoch{};
    // See MountRequest::IsRefresh. Also set when the server tells a player who loads the pair later.
    bool IsRefresh{};
};
