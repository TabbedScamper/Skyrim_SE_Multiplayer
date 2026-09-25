#include <Messages/NotifyCameraState.h>

void NotifyCameraState::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Snapshot.Serialize(aWriter);
}

void NotifyCameraState::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    Snapshot.Deserialize(aReader);
}
