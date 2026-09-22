#include <Messages/PartyStartRequest.h>
#include <TiltedCore/Serialization.hpp>

void PartyStartRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Mode, 2);
    TiltedPhoques::Serialization::WriteBool(aWriter, Launch);
    TiltedPhoques::Serialization::WriteString(aWriter, CheckpointId);
}

void PartyStartRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t value{};
    aReader.ReadBits(value, 2);
    Mode = value & 0x3;
    Launch = TiltedPhoques::Serialization::ReadBool(aReader);
    CheckpointId = TiltedPhoques::Serialization::ReadString(aReader);
}
