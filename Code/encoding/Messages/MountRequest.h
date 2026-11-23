#pragma once

#include "Message.h"

struct MountRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kMountRequest;

    MountRequest()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const MountRequest& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && RiderId == acRhs.RiderId && RiderOwnershipEpoch == acRhs.RiderOwnershipEpoch && MountId == acRhs.MountId &&
               MountOwnershipEpoch == acRhs.MountOwnershipEpoch && IsRefresh == acRhs.IsRefresh;
    }

    uint32_t RiderId{};
    uint32_t RiderOwnershipEpoch{};
    uint32_t MountId{};
    uint32_t MountOwnershipEpoch{};
    // The rider was already on: said again for games that load the pair later, or by its new owner. Their copies get on at once, with no climb.
    bool IsRefresh{};
};
