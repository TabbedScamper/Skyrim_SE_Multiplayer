#include <Messages/SubtitleRequest.h>

void SubtitleRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    Serialization::WriteVarInt(aWriter, Tick);
    Serialization::WriteString(aWriter, Text);
    Serialization::WriteVarInt(aWriter, TopicFormId);
}

void SubtitleRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Tick = Serialization::ReadVarInt(aReader);
    Text = Serialization::ReadString(aReader);
    TopicFormId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}
