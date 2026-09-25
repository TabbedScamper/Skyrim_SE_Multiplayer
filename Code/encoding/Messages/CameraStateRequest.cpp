#include <Messages/CameraStateRequest.h>

void CameraStateRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Snapshot.Serialize(aWriter);
}

void CameraStateRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    Snapshot.Deserialize(aReader);
}
