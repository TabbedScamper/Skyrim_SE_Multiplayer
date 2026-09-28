#include <Messages/NotifyAnimObject.h>

void NotifyAnimObject::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Id);
    aWriter.WriteBits(Kind, 8);
    AnimObject.Serialize(aWriter);
}

void NotifyAnimObject::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    Id = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    uint64_t kind{};
    aReader.ReadBits(kind, 8);
    Kind = static_cast<uint8_t>(kind);
    AnimObject.Deserialize(aReader);
}
