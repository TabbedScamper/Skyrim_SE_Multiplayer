#include <Messages/AuthenticationResponse.h>

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
    Type = static_cast<ResponseType>(Serialization::ReadVarInt(aReader) & 0xFFFFFFFF);
    SKSEActive = Serialization::ReadBool(aReader);
    MO2Active = Serialization::ReadBool(aReader);
    Version = Serialization::ReadString(aReader);
    UserMods.Deserialize(aReader);
    Settings.Deserialize(aReader);
    PlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    CampaignId = Serialization::ReadString(aReader);
    aReader.ReadBits(CampaignRevision, 64);
    aReader.ReadBits(AuthorityEpoch, 64);
}
