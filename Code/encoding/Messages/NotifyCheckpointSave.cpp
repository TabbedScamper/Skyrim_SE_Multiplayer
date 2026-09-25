#include <Messages/NotifyCheckpointSave.h>
#include <TiltedCore/Serialization.hpp>

void NotifyCheckpointSave::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    TiltedPhoques::Serialization::WriteString(aWriter, CheckpointId);
    aWriter.WriteBits(AuthorityEpoch, 64);
}

void NotifyCheckpointSave::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    CheckpointId = TiltedPhoques::Serialization::ReadString(aReader);
    aReader.ReadBits(AuthorityEpoch, 64);
}
