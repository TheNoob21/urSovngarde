#pragma once

#include <DecodeGuard.h>
#include <common/ExtensionApi.h>

struct ExtensionData
{
    static constexpr uint32_t kSubscribe = 8;
    static constexpr uint32_t kResubscribe = 13;
    uint32_t Addon{};
    uint32_t Version{};
    uint32_t Scope{};
    uint32_t State{};
    uint32_t Subject{};
    uint32_t SubjectEpoch{};
    uint32_t Target{};
    uint32_t TargetEpoch{};
    uint64_t Key{};
    uint64_t Revision{};
    uint64_t WriterHigh{};
    uint64_t WriterLow{};
    uint32_t Result{};
    TiltedPhoques::Vector<uint8_t> Bytes;

    bool IsAreaState() const noexcept { return State == UR_EXTENSION_AREA_REPLACE || State == UR_EXTENSION_AREA_ERASE; }
    bool IsLease() const noexcept { return State == UR_EXTENSION_CLAIM || State == UR_EXTENSION_RELEASE; }
    bool IsReservation() const noexcept { return State == UR_EXTENSION_RESERVE || State == UR_EXTENSION_UNRESERVE; }
    bool IsReplacement() const noexcept { return State == UR_EXTENSION_REPLACE || State == UR_EXTENSION_REPLACE_OWNED_TARGET; }
    bool IsErase() const noexcept { return State == UR_EXTENSION_ERASE || State == UR_EXTENSION_ERASE_OWNED_TARGET; }
    bool RequiresOwnedTarget() const noexcept { return State == UR_EXTENSION_REPLACE_OWNED_TARGET || State == UR_EXTENSION_ERASE_OWNED_TARGET; }
    bool IsDurable() const noexcept { return State >= UR_EXTENSION_STORE && State <= UR_EXTENSION_READ; }
    bool Valid() const noexcept;
    bool ValidRequest() const noexcept;
    bool ValidNotification(uint32_t aSender) const noexcept;
    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
};
