#include <Messages/LeaderControlRequest.h>
#include <TiltedCore/Serialization.hpp>

void LeaderControlRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteBool(aWriter, FreeControl);
}

void LeaderControlRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    FreeControl = Serialization::ReadBool(aReader);
}
