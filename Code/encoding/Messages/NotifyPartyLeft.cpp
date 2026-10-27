#include <Messages/NotifyPartyLeft.h>
#include <TiltedCore/Serialization.hpp>

void NotifyPartyLeft::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteBool(aWriter, Forced);
}

void NotifyPartyLeft::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    Forced = Serialization::ReadBool(aReader);
}
