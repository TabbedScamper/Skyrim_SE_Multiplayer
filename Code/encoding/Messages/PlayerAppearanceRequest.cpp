#include <Messages/PlayerAppearanceRequest.h>
#include <TiltedCore/Serialization.hpp>

void PlayerAppearanceRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    aWriter.WriteBits(ChangeFlags, 32);
    Serialization::WriteString(aWriter, AppearanceBuffer);
    FaceTints.Serialize(aWriter);
    Serialization::WriteBool(aWriter, InCreator);
}

void PlayerAppearanceRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    ServerId = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
    uint64_t flags = 0;
    aReader.ReadBits(flags, 32);
    ChangeFlags = static_cast<uint32_t>(flags & 0xFFFFFFFF);
    AppearanceBuffer = Serialization::ReadString(aReader);
    FaceTints.Deserialize(aReader);
    InCreator = Serialization::ReadBool(aReader);
}
