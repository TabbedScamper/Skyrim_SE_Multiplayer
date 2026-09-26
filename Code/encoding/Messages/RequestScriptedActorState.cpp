#include <Messages/RequestScriptedActorState.h>

void RequestScriptedActorState::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    State.Serialize(aWriter);
    Anchor.Serialize(aWriter);
}

void RequestScriptedActorState::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    State.Deserialize(aReader);
    Anchor.Deserialize(aReader);
}
