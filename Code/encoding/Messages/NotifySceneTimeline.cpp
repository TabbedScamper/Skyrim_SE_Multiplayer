#include <Messages/NotifySceneTimeline.h>

void NotifySceneTimeline::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Snapshot.Serialize(aWriter);
}

void NotifySceneTimeline::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    Snapshot.Deserialize(aReader);
}
