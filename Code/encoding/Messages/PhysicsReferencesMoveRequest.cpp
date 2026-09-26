#include <Messages/PhysicsReferencesMoveRequest.h>
#include <TiltedCore/Serialization.hpp>
#include <Structs/CheckedRead.h>

void PhysicsReferencesMoveRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Tick);
    Serialization::WriteVarInt(aWriter, Updates.size());
    for (const auto& update : Updates)
        update.Serialize(aWriter);
}

void PhysicsReferencesMoveRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    m_valid = false;
    Updates.clear();
    try
    {
        ClientMessage::DeserializeRaw(aReader);
        Tick = CheckedRead::VarInt(aReader);
        const auto count = CheckedRead::VarInt(aReader);
        if (count > PhysicsReferenceUpdate::MaxUpdates ||
            count > CheckedRead::RemainingBits(aReader) / PhysicsReferenceUpdate::MinBits)
            throw std::runtime_error("reference update count exceeds limit");
        Updates.resize(count);
        for (auto& update : Updates)
            update.Deserialize(aReader);
        m_valid = true;
    }
    catch (...)
    {
        Updates.clear();
    }
}
