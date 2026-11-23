#pragma once

#include <DecodeGuard.h>

#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <cmath>
#include <cstdint>

//! Furniture placed at run time (a 0xFF reference, by a script or a mod) has no id the games share. Its seat is named by its base
//! furniture form and where the reference stands, in whole units; each game finds its own reference of that base there.
struct RuntimeSeat
{
    //! Set for a run-time reference: the seat's form names its base furniture, not the reference.
    bool Placed{};
    int32_t X{};
    int32_t Y{};
    int32_t Z{};

    bool operator==(const RuntimeSeat&) const noexcept = default;
};

namespace RuntimeSeats
{
//! Coordinates beyond this are none any world has: they are sent as 0, and a message with one fails.
constexpr int32_t kMaxCoordinate = 1 << 24;
//! The world's range, as the movement code takes it (ClientTrust::kMaxCoordinate): a place beyond it seats nobody.
constexpr int32_t kWorldCoordinate = 5000000;

//! Whether a seat is placed at run time at a place within the world's range.
inline bool InWorld(const RuntimeSeat& acSeat) noexcept
{
    const auto cIn = [](const int32_t aValue)
    {
        return aValue >= -kWorldCoordinate && aValue <= kWorldCoordinate;
    };
    return acSeat.Placed && cIn(acSeat.X) && cIn(acSeat.Y) && cIn(acSeat.Z);
}

//! A position in whole units. Out of range or not a number is 0.
inline int32_t Round(const float aValue) noexcept
{
    return std::isfinite(aValue) && std::fabs(aValue) < static_cast<float>(kMaxCoordinate) ? static_cast<int32_t>(std::lround(aValue)) : 0;
}

inline RuntimeSeat At(const float aX, const float aY, const float aZ) noexcept
{
    return {true, Round(aX), Round(aY), Round(aZ)};
}

inline uint64_t Zig(const int32_t aValue) noexcept
{
    return (static_cast<uint64_t>(static_cast<int64_t>(aValue)) << 1) ^ static_cast<uint64_t>(static_cast<int64_t>(aValue) >> 63);
}

//! A zigzagged coordinate; out of range fails the message.
inline int32_t Unzig(const uint64_t aValue) noexcept
{
    if (!DecodeGuard::Within(aValue, static_cast<uint64_t>(kMaxCoordinate) * 2))
        return 0;
    return static_cast<int32_t>(static_cast<int64_t>(aValue >> 1) ^ -static_cast<int64_t>(aValue & 1));
}

//! 0 for a seat in a reference the games share; 1 and the three coordinates for one placed at run time.
inline void Write(TiltedPhoques::Buffer::Writer& aWriter, const RuntimeSeat& acSeat) noexcept
{
    TiltedPhoques::Serialization::WriteVarInt(aWriter, acSeat.Placed ? 1 : 0);
    if (!acSeat.Placed)
        return;
    TiltedPhoques::Serialization::WriteVarInt(aWriter, Zig(acSeat.X));
    TiltedPhoques::Serialization::WriteVarInt(aWriter, Zig(acSeat.Y));
    TiltedPhoques::Serialization::WriteVarInt(aWriter, Zig(acSeat.Z));
}

inline RuntimeSeat Read(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    RuntimeSeat seat{};
    const uint64_t cPlaced = TiltedPhoques::Serialization::ReadVarInt(aReader);
    if (!DecodeGuard::Within(cPlaced, 1) || cPlaced == 0)
        return seat;
    seat.Placed = true;
    seat.X = Unzig(TiltedPhoques::Serialization::ReadVarInt(aReader));
    seat.Y = Unzig(TiltedPhoques::Serialization::ReadVarInt(aReader));
    seat.Z = Unzig(TiltedPhoques::Serialization::ReadVarInt(aReader));
    return seat;
}
} // namespace RuntimeSeats
