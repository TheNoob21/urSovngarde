#include <Structs/ExtensionData.h>

static_assert(UR_EXTENSION_MAX_BYTES == 8192 && UR_EXTENSION_RANGE == 0 && UR_EXTENSION_PARTY == 1 && UR_EXTENSION_OWNER == 2);
static_assert(
    UR_EXTENSION_EVENT == 0 && UR_EXTENSION_REPLACE == 1 && UR_EXTENSION_ERASE == 2 && UR_EXTENSION_STORE == 3 && UR_EXTENSION_REMOVE == 4 && UR_EXTENSION_READ == 5 &&
    UR_EXTENSION_CLAIM == 6 && UR_EXTENSION_RELEASE == 7 && UR_EXTENSION_REPLACE_OWNED_TARGET == 9 && UR_EXTENSION_ERASE_OWNED_TARGET == 10 && UR_EXTENSION_RESERVE == 11 &&
    UR_EXTENSION_UNRESERVE == 12 && UR_EXTENSION_AREA_REPLACE == 14 && UR_EXTENSION_AREA_ERASE == 15);

bool ExtensionData::Valid() const noexcept
{
    if (!Addon || !Version || Scope > UR_EXTENSION_OWNER || State > UR_EXTENSION_AREA_ERASE || Bytes.size() > UR_EXTENSION_MAX_BYTES || Result > 2)
        return false;
    if ((bool(Subject) != bool(SubjectEpoch)) || (bool(Target) != bool(TargetEpoch)) || (Scope == UR_EXTENSION_OWNER && !Target))
        return false;
    if (State == kSubscribe || State == kResubscribe)
        return Scope == UR_EXTENSION_RANGE && !Subject && !Target && !Key && !Revision && !WriterHigh && !WriterLow && !Result && Bytes.empty();
    if (IsDurable())
        return Scope == UR_EXTENSION_RANGE && Key && !Subject && !Target;
    if (IsAreaState())
        return Scope == UR_EXTENSION_RANGE && !Target && !Revision && !WriterHigh && !WriterLow && !Result && (State == UR_EXTENSION_AREA_REPLACE ? !Bytes.empty() : Bytes.empty());
    if (IsReservation())
        return Scope == UR_EXTENSION_RANGE && !Target && Key && !Revision && !WriterHigh && !WriterLow;
    if (IsLease())
        return Scope == UR_EXTENSION_OWNER && Key && !Revision && !WriterHigh && !WriterLow && (Result == 2 ? Bytes.empty() : !Result && !Bytes.empty());
    if (Revision || WriterHigh || WriterLow || Result)
        return false;
    if (State == UR_EXTENSION_EVENT)
        return !Key && !Bytes.empty();
    if (RequiresOwnedTarget() && (!Target || Scope != UR_EXTENSION_RANGE))
        return false;
    return Key && Scope != UR_EXTENSION_OWNER && (IsErase() || !Bytes.empty());
}

bool ExtensionData::ValidRequest() const noexcept
{
    return Valid() && !Result && (!IsAreaState() || Subject) && (!IsDurable() || ((WriterHigh || WriterLow) && (State == UR_EXTENSION_STORE ? !Bytes.empty() : Bytes.empty())));
}

bool ExtensionData::ValidNotification(uint32_t aSender) const noexcept
{
    return Valid() && State != kSubscribe && State != kResubscribe && !WriterHigh && !WriterLow &&
           (IsAreaState() ? (!aSender && !Subject && Key) : IsDurable() || (aSender && Subject));
}

void ExtensionData::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(1, 8);
    for (const auto value : {Addon, Version, Scope, State, Subject, SubjectEpoch, Target, TargetEpoch})
        aWriter.WriteBits(value, 32);
    aWriter.WriteBits(Key, 64);
    if (IsDurable())
    {
        aWriter.WriteBits(Revision, 64);
        aWriter.WriteBits(WriterHigh, 64);
        aWriter.WriteBits(WriterLow, 64);
        aWriter.WriteBits(Result, 8);
    }
    else if (IsReservation() || IsLease())
        aWriter.WriteBits(Result, 8);
    aWriter.WriteBits(Bytes.size(), 16);
    if (!Bytes.empty())
        aWriter.WriteBytes(Bytes.data(), Bytes.size());
}

void ExtensionData::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t wire{};
    if (!aReader.ReadBits(wire, 8) || wire != 1)
    {
        DecodeGuard::Fail();
        return;
    }
    for (auto* pField : {&Addon, &Version, &Scope, &State, &Subject, &SubjectEpoch, &Target, &TargetEpoch})
    {
        if (!aReader.ReadBits(wire, 32))
        {
            DecodeGuard::Fail();
            return;
        }
        *pField = static_cast<uint32_t>(wire);
    }
    if (!aReader.ReadBits(Key, 64))
    {
        DecodeGuard::Fail();
        return;
    }
    if (IsDurable())
    {
        if (!aReader.ReadBits(Revision, 64) || !aReader.ReadBits(WriterHigh, 64) || !aReader.ReadBits(WriterLow, 64) || !aReader.ReadBits(wire, 8))
        {
            DecodeGuard::Fail();
            return;
        }
        Result = static_cast<uint32_t>(wire);
    }
    else if (IsReservation() || IsLease())
    {
        if (!aReader.ReadBits(wire, 8))
        {
            DecodeGuard::Fail();
            return;
        }
        Result = static_cast<uint32_t>(wire);
    }
    if (!aReader.ReadBits(wire, 16) || !DecodeGuard::Within(wire, UR_EXTENSION_MAX_BYTES))
    {
        DecodeGuard::Fail();
        return;
    }
    Bytes.resize(static_cast<size_t>(wire));
    if ((!Bytes.empty() && !aReader.ReadBytes(Bytes.data(), Bytes.size())) || !Valid())
        DecodeGuard::Fail();
}
