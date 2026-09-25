#include <Messages/NotifyPhysicsReferencesMove.h>
#include <TiltedCore/Serialization.hpp>

void NotifyPhysicsReferencesMove::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Tick);
    Serialization::WriteVarInt(aWriter, AuthorityEpoch);
    Serialization::WriteVarInt(aWriter, Updates.size());
    for (const auto& update : Updates)
        update.Serialize(aWriter);
}

void NotifyPhysicsReferencesMove::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    Tick = Serialization::ReadVarInt(aReader);
    AuthorityEpoch = Serialization::ReadVarInt(aReader);
    const auto count = Serialization::ReadVarInt(aReader);
    Updates.resize(count);
    for (auto& update : Updates)
        update.Deserialize(aReader);
}
