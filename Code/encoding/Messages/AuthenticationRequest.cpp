#include <Messages/AuthenticationRequest.h>
#include <Structs/CheckedRead.h>

void AuthenticationRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, DiscordId);
    Serialization::WriteBool(aWriter, SKSEActive);
    Serialization::WriteBool(aWriter, MO2Active);
    Serialization::WriteString(aWriter, Token);
    Serialization::WriteString(aWriter, Version);
    UserMods.Serialize(aWriter);
    Serialization::WriteString(aWriter, Username);
    WorldSpaceId.Serialize(aWriter);
    CellId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, Level);
    PlayerTime.Serialize(aWriter);
}

void AuthenticationRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    m_valid = false;
    try
    {
        ClientMessage::DeserializeRaw(aReader);

        DiscordId = CheckedRead::VarInt(aReader);
        SKSEActive = CheckedRead::Bool(aReader);
        MO2Active = CheckedRead::Bool(aReader);
        Token = CheckedRead::String(aReader);
        Version = CheckedRead::String(aReader);
        if (!UserMods.Deserialize(aReader))
            return;
        Username = CheckedRead::String(aReader);
        WorldSpaceId.Deserialize(aReader);
        CellId.Deserialize(aReader);
        Level = CheckedRead::VarInt(aReader) & 0xFFFF;
        PlayerTime.Deserialize(aReader);
        m_valid = true;
    }
    catch (...)
    {
    }
}
