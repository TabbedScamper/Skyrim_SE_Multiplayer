#include <Messages/PhysicsReferencesMoveRequest.h>
#include <TiltedCore/Serialization.hpp>

void PhysicsReferencesMoveRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Tick);
    Serialization::WriteVarInt(aWriter, Updates.size());
    for (const auto& update : Updates)
        update.Serialize(aWriter);
}

void PhysicsReferencesMoveRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    Tick = Serialization::ReadVarInt(aReader);
    const auto count = Serialization::ReadVarInt(aReader);
    Updates.resize(count);
    for (auto& update : Updates)
        update.Deserialize(aReader);
}
