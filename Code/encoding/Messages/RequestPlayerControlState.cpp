#include <Messages/RequestPlayerControlState.h>

void RequestPlayerControlState::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    State.Serialize(aWriter);
}

void RequestPlayerControlState::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    m_valid = State.Deserialize(aReader);
}
