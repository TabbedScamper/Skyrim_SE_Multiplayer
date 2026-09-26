#include <Messages/AuthenticationResponse.h>
#include <Structs/CheckedRead.h>

void AuthenticationResponse::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, static_cast<uint32_t>(Type));
    Serialization::WriteBool(aWriter, SKSEActive);
    Serialization::WriteBool(aWriter, MO2Active);
    Serialization::WriteString(aWriter, Version);
    UserMods.Serialize(aWriter);
    Settings.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, PlayerId);
    Serialization::WriteString(aWriter, CampaignId);
    aWriter.WriteBits(CampaignRevision, 64);
    aWriter.WriteBits(AuthorityEpoch, 64);
}

void AuthenticationResponse::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    m_valid = false;
    try
    {
        Type = static_cast<ResponseType>(CheckedRead::VarInt(aReader) & 0xFFFFFFFF);
        SKSEActive = CheckedRead::Bool(aReader);
        MO2Active = CheckedRead::Bool(aReader);
        Version = CheckedRead::String(aReader);
        if (!UserMods.Deserialize(aReader))
            return;
        Settings.Deserialize(aReader);
        PlayerId = CheckedRead::VarInt(aReader) & 0xFFFFFFFF;
        CampaignId = CheckedRead::String(aReader);
        CheckedRead::Bits(aReader, CampaignRevision, 64);
        CheckedRead::Bits(aReader, AuthorityEpoch, 64);
        m_valid = true;
    }
    catch (...)
    {
    }
}
