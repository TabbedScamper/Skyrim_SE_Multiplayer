#include <Messages/CheckpointSaveRequest.h>
#include <TiltedCore/Serialization.hpp>

void CheckpointSaveRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    TiltedPhoques::Serialization::WriteString(aWriter, CheckpointId);
}

void CheckpointSaveRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    CheckpointId = TiltedPhoques::Serialization::ReadString(aReader);
}
