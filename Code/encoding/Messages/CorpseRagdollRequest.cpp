#include <Messages/CorpseRagdollRequest.h>
#include <TiltedCore/Serialization.hpp>

namespace CorpseRagdollEncoding
{
void WriteBodies(TiltedPhoques::Buffer::Writer& aWriter, const TiltedPhoques::Vector<CorpseRagdollBody>& acBodies) noexcept
{
    const auto count = (std::min)(static_cast<uint32_t>(acBodies.size()), CorpseRagdollRequest::kMaxBodies);
    TiltedPhoques::Serialization::WriteVarInt(aWriter, count);
    for (uint32_t i = 0; i < count; ++i)
    {
        for (const float value : acBodies[i].Position)
            TiltedPhoques::Serialization::WriteFloat(aWriter, value);
        for (const float value : acBodies[i].Rotation)
            TiltedPhoques::Serialization::WriteFloat(aWriter, value);
    }
}

void ReadBodies(TiltedPhoques::Buffer::Reader& aReader, TiltedPhoques::Vector<CorpseRagdollBody>& aBodies) noexcept
{
    const auto count = (std::min)(static_cast<uint32_t>(TiltedPhoques::Serialization::ReadVarInt(aReader)),
        CorpseRagdollRequest::kMaxBodies);
    aBodies.resize(count);
    for (auto& body : aBodies)
    {
        for (float& value : body.Position)
            value = TiltedPhoques::Serialization::ReadFloat(aReader);
        for (float& value : body.Rotation)
            value = TiltedPhoques::Serialization::ReadFloat(aReader);
    }
}
} // namespace CorpseRagdollEncoding

void CorpseRagdollRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    TiltedPhoques::Serialization::WriteVarInt(aWriter, ServerId);
    aWriter.WriteBits(Tick, 64);
    CorpseRagdollEncoding::WriteBodies(aWriter, Bodies);
}

void CorpseRagdollRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    ServerId = static_cast<uint32_t>(TiltedPhoques::Serialization::ReadVarInt(aReader));
    aReader.ReadBits(Tick, 64);
    CorpseRagdollEncoding::ReadBodies(aReader, Bodies);
}
