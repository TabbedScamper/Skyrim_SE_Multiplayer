#include <Messages/NotifyDialogueListen.h>
void NotifyDialogueListen::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Speaker, 32); State.Serialize(aWriter);
}
void NotifyDialogueListen::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    uint64_t value{};
    m_valid = aReader.ReadBits(value, 32); Speaker = static_cast<uint32_t>(value);
    m_valid = m_valid && State.Deserialize(aReader);
}
