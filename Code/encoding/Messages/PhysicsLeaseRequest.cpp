#include <Messages/PhysicsLeaseRequest.h>
#include <TiltedCore/Serialization.hpp>
#include <Structs/CheckedRead.h>

void PhysicsLeaseRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Id.Serialize(aWriter);
    TiltedPhoques::Serialization::WriteBool(aWriter, Hold);
    aWriter.WriteBits(Epoch, 64);
}

void PhysicsLeaseRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    m_valid = false;
    try
    {
        ClientMessage::DeserializeRaw(aReader);
        Id.Deserialize(aReader);
        Hold = CheckedRead::Bool(aReader);
        CheckedRead::Bits(aReader, Epoch, 64);
        m_valid = static_cast<bool>(Id);
    }
    catch (...)
    {
        m_valid = false;
    }
}
