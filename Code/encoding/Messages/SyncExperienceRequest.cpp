#include <Messages/SyncExperienceRequest.h>

void SyncExperienceRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Skill);
    Serialization::WriteFloat(aWriter, Experience);
}

void SyncExperienceRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Skill = Serialization::ReadVarInt(aReader) & 0xFF;
    Experience = Serialization::ReadFloat(aReader);
}
