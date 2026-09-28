#include <Messages/AnimObjectRequest.h>

void AnimObjectRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Id);
    aWriter.WriteBits(Kind, 8);
    AnimObject.Serialize(aWriter);
}

void AnimObjectRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Id = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    uint64_t kind{};
    aReader.ReadBits(kind, 8);
    Kind = static_cast<uint8_t>(kind);
    AnimObject.Deserialize(aReader);
}
