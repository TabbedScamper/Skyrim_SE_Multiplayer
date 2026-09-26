#include <Messages/RequestPartyUnstuck.h>

void RequestPartyUnstuck::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Move.Serialize(aWriter);
}

void RequestPartyUnstuck::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    m_valid = Move.Deserialize(aReader);
}
