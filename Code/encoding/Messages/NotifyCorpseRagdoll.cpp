#include <Messages/NotifyCorpseRagdoll.h>
#include <TiltedCore/Serialization.hpp>

void NotifyCorpseRagdoll::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    TiltedPhoques::Serialization::WriteVarInt(aWriter, ServerId);
    aWriter.WriteBits(Tick, 64);
    for (const float value : Origin)
        TiltedPhoques::Serialization::WriteFloat(aWriter, value);
    CorpseRagdollEncoding::WriteBodies(aWriter, Bodies);
}

void NotifyCorpseRagdoll::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    ServerId = static_cast<uint32_t>(TiltedPhoques::Serialization::ReadVarInt(aReader));
    aReader.ReadBits(Tick, 64);
    for (float& value : Origin)
        value = TiltedPhoques::Serialization::ReadFloat(aReader);
    CorpseRagdollEncoding::ReadBodies(aReader, Bodies);
}
