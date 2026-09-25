#include <Messages/NotifyCorpseRagdoll.h>
#include <TiltedCore/Serialization.hpp>

void NotifyCorpseRagdoll::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    TiltedPhoques::Serialization::WriteVarInt(aWriter, ServerId);
    CorpseRagdollEncoding::WriteBodies(aWriter, Bodies);
}

void NotifyCorpseRagdoll::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    ServerId = static_cast<uint32_t>(TiltedPhoques::Serialization::ReadVarInt(aReader));
    CorpseRagdollEncoding::ReadBodies(aReader, Bodies);
}
