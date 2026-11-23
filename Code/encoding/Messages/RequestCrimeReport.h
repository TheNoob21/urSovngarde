#pragma once

#include "Message.h"

#include <Structs/CrimeReport.h>

//! This player's crime that NPC copies here saw: for the games that run those NPCs.
struct RequestCrimeReport final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestCrimeReport;

    RequestCrimeReport()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { CrimeReportLimits::Write(aWriter, Report); }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ClientMessage::DeserializeRaw(aReader);
        CrimeReportLimits::Read(aReader, Report);
    }

    bool operator==(const RequestCrimeReport& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Report == acRhs.Report; }

    CrimeReport Report{};
};
