#include <Messages/EnterInteriorCellRequest.h>
#include <TiltedCore/Serialization.hpp>

void EnterInteriorCellRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    CellId.Serialize(aWriter);
    Serialization::WriteBool(aWriter, Heartbeat);
}

void EnterInteriorCellRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    CellId.Deserialize(aReader);
    Heartbeat = Serialization::ReadBool(aReader);
}
