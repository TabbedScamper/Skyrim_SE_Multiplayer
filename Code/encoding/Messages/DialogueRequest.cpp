#include <Messages/DialogueRequest.h>

void DialogueRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    Serialization::WriteVarInt(aWriter, Tick);
    Serialization::WriteString(aWriter, SoundFilename);
}

void DialogueRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Tick = Serialization::ReadVarInt(aReader);
    SoundFilename = Serialization::ReadString(aReader);
}
