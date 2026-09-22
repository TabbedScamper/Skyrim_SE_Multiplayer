#include <Messages/PartySessionSettingsRequest.h>
#include <TiltedCore/Serialization.hpp>

void PartySessionSettingsRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteBool(aWriter, Open);
    Serialization::WriteString(aWriter, Password);
}

void PartySessionSettingsRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Open = Serialization::ReadBool(aReader);
    Password = Serialization::ReadString(aReader);
}
