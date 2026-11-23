#include <Messages/AuthenticationRequest.h>
#include <BoundedString.h>
#include <DecodeGuard.h>

void AuthenticationRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, DiscordId);
    Serialization::WriteBool(aWriter, SKSEActive);
    Serialization::WriteBool(aWriter, MO2Active);
    Serialization::WriteString(aWriter, Token);
    Serialization::WriteString(aWriter, Version);
    Serialization::WriteString(aWriter, Protocol);
    UserMods.Serialize(aWriter);
    Serialization::WriteString(aWriter, Username);
    WorldSpaceId.Serialize(aWriter);
    CellId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, Level);
    PlayerTime.Serialize(aWriter);
    Serialization::WriteBool(aWriter, HideFromPublicPage);
    ContentSizes.Serialize(aWriter);
}

void AuthenticationRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    DiscordId = Serialization::ReadVarInt(aReader);
    SKSEActive = Serialization::ReadBool(aReader);
    MO2Active = Serialization::ReadBool(aReader);
    // The login is read before the server knows who sent it: each text is checked against a real length first.
    constexpr uint64_t cMaxToken = 1024;
    constexpr uint64_t cMaxShortText = 256;
    Token = BoundedString::Read(aReader, cMaxToken);
    Version = BoundedString::Read(aReader, cMaxShortText);
    Protocol = BoundedString::Read(aReader, cMaxShortText);
    if (DecodeGuard::t_failed)
        return;
    UserMods.Deserialize(aReader);
    if (DecodeGuard::t_failed)
        return;
    Username = BoundedString::Read(aReader, cMaxShortText);
    if (DecodeGuard::t_failed)
        return;
    WorldSpaceId.Deserialize(aReader);
    CellId.Deserialize(aReader);
    Level = Serialization::ReadVarInt(aReader) & 0xFFFF;
    PlayerTime.Deserialize(aReader);
    HideFromPublicPage = Serialization::ReadBool(aReader);
    ContentSizes.Deserialize(aReader);
}
