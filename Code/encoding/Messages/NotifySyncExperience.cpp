#include <Messages/NotifySyncExperience.h>

void NotifySyncExperience::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Skill);
    Serialization::WriteFloat(aWriter, Experience);
}

void NotifySyncExperience::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    Skill = Serialization::ReadVarInt(aReader) & 0xFF;
    Experience = Serialization::ReadFloat(aReader);
}
