#include <Messages/NotifySaidOnce.h>

void NotifySaidOnce::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Info.Serialize(aWriter);
}

void NotifySaidOnce::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Info.Deserialize(aReader);
}
