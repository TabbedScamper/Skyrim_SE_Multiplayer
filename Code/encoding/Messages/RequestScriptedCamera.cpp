#include <Messages/RequestScriptedCamera.h>

void RequestScriptedCamera::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    State.Serialize(aWriter);
}

void RequestScriptedCamera::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    m_valid = State.Deserialize(aReader);
}
