#pragma once

#include <DecodeGuard.h>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <Structs/GameId.h>
#include <Structs/RuntimeSeat.h>

namespace PlayerFurniture
{
// TESFurniture::ActiveMarker contains the 24 enabled marker bits.
constexpr uint32_t kMarkerCount = 24;

enum class Action : uint8_t
{
    Enter,
    Sit,
    Sleep,
    Refresh,
    Exit,
    Cancel,
};

enum class Verdict : uint8_t
{
    Rejected,
    Occupied,
    Granted,
    Settled,
    Refreshed,
    Released,
};

enum class SeatState : uint8_t
{
    Standing = 0,
    Sitting = 3,
    Sleeping = 7,
};

struct Seat
{
    //! The furniture reference, or its base furniture form for one placed at run time (Placement).
    GameId Reference{};
    uint32_t Marker{};
    RuntimeSeat Placement{};

    bool operator==(const Seat&) const noexcept = default;
};

struct Request
{
    uint32_t Actor{};
    uint32_t Epoch{};
    uint64_t Sequence{};
    Action Change{};
    Seat Where{};
    uint64_t Lease{};
    uint64_t EnterSequence{};

    bool operator==(const Request&) const noexcept = default;
};

inline uint32_t ReadField(Buffer::Reader& aReader, const uint64_t aMax = UINT32_MAX) noexcept
{
    const uint64_t cValue = TiltedPhoques::Serialization::ReadVarInt(aReader);
    return DecodeGuard::Within(cValue, aMax) ? static_cast<uint32_t>(cValue) : 0;
}

inline void WriteSeat(Buffer::Writer& aWriter, const Seat& acSeat) noexcept
{
    acSeat.Reference.Serialize(aWriter);
    TiltedPhoques::Serialization::WriteVarInt(aWriter, acSeat.Marker);
    RuntimeSeats::Write(aWriter, acSeat.Placement);
}

inline Seat ReadSeat(Buffer::Reader& aReader) noexcept
{
    const uint32_t cBase = ReadField(aReader, 0xFFFFFF);
    const uint32_t cMod = ReadField(aReader, UINT16_MAX);
    const uint32_t cMarker = ReadField(aReader, kMarkerCount - 1);
    return {GameId{cMod, cBase}, cMarker, RuntimeSeats::Read(aReader)};
}
} // namespace PlayerFurniture
