#include <Messages/NotifyDialogue.h>

void NotifyDialogue::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    Serialization::WriteVarInt(aWriter, Tick);
    Serialization::WriteString(aWriter, SoundFilename);
}

void NotifyDialogue::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Tick = Serialization::ReadVarInt(aReader);
    SoundFilename = Serialization::ReadString(aReader);
}
