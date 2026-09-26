#include <Messages/NotifyPhysicsReferencesMove.h>
#include <TiltedCore/Serialization.hpp>
#include <Structs/CheckedRead.h>

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
    m_valid = false;
    Updates.clear();
    try
    {
        ServerMessage::DeserializeRaw(aReader);
        Tick = CheckedRead::VarInt(aReader);
        AuthorityEpoch = CheckedRead::VarInt(aReader);
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
