#include <Messages/PartyReadyRequest.h>
#include <TiltedCore/Serialization.hpp>

void PartyReadyRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    TiltedPhoques::Serialization::WriteBool(aWriter, Ready);
}

void PartyReadyRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Ready = TiltedPhoques::Serialization::ReadBool(aReader);
}
