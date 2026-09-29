#include <Messages/NotifyPhysicsLease.h>
#include <TiltedCore/Serialization.hpp>
#include <Structs/CheckedRead.h>
#include <limits>

void NotifyPhysicsLease::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Id.Serialize(aWriter);
    TiltedPhoques::Serialization::WriteVarInt(aWriter, HolderId);
    aWriter.WriteBits(Epoch, 64);
}

void NotifyPhysicsLease::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    m_valid = false;
    try
    {
        ServerMessage::DeserializeRaw(aReader);
        Id.Deserialize(aReader);
        const auto holder = CheckedRead::VarInt(aReader);
        CheckedRead::Bits(aReader, Epoch, 64);
        HolderId = static_cast<uint32_t>(holder);
        m_valid = static_cast<bool>(Id) && holder <= (std::numeric_limits<uint32_t>::max)();
    }
    catch (...)
    {
        m_valid = false;
    }
}
