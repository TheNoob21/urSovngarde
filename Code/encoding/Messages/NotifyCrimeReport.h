#pragma once

#include "Message.h"

#include <Structs/CrimeReport.h>

//! To the game that runs the witnesses: another player's crime they saw, with only that game's own witnesses in it.
struct NotifyCrimeReport final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyCrimeReport;

    NotifyCrimeReport()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override
    {
        Serialization::WriteVarInt(aWriter, OffenderPlayerId);
        CrimeReportLimits::Write(aWriter, Report);
    }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ServerMessage::DeserializeRaw(aReader);
        OffenderPlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        CrimeReportLimits::Read(aReader, Report);
    }

    bool operator==(const NotifyCrimeReport& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && OffenderPlayerId == acRhs.OffenderPlayerId && Report == acRhs.Report;
    }

    //! The player who did it: the sender, as the server knows it.
    uint32_t OffenderPlayerId{};
    CrimeReport Report{};
};
