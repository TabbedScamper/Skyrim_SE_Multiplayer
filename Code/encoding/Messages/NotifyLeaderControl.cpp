#include <Messages/NotifyLeaderControl.h>
#include <TiltedCore/Serialization.hpp>

void NotifyLeaderControl::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteBool(aWriter, FreeControl);
}

void NotifyLeaderControl::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    FreeControl = Serialization::ReadBool(aReader);
}
