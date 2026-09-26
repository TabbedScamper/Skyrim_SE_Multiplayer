#include <Messages/NotifyScriptedActorState.h>

void NotifyScriptedActorState::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    State.Serialize(aWriter);
    FormId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, LeaderPlayerId);
}

void NotifyScriptedActorState::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    State.Deserialize(aReader);
    FormId.Deserialize(aReader);
    LeaderPlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}
