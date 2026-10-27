#include <Messages/RequestSaidOnce.h>

void RequestSaidOnce::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Info.Serialize(aWriter);
}

void RequestSaidOnce::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Info.Deserialize(aReader);
}
