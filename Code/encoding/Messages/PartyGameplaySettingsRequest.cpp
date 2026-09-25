#include <Messages/PartyGameplaySettingsRequest.h>
#include <TiltedCore/Serialization.hpp>

void PartyGameplaySettingsRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    TiltedPhoques::Serialization::WriteVarInt(aWriter, Difficulty);
    TiltedPhoques::Serialization::WriteBool(aWriter, PvpEnabled);
}

void PartyGameplaySettingsRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Difficulty = static_cast<uint32_t>(TiltedPhoques::Serialization::ReadVarInt(aReader));
    PvpEnabled = TiltedPhoques::Serialization::ReadBool(aReader);
}
