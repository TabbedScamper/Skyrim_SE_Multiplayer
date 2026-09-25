#include <Messages/SceneTimelineRequest.h>

void SceneTimelineRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Snapshot.Serialize(aWriter);
}

void SceneTimelineRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    Snapshot.Deserialize(aReader);
}
