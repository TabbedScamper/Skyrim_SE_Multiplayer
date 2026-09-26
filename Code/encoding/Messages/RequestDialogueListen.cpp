#include <Messages/RequestDialogueListen.h>
void RequestDialogueListen::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Query, 1); aWriter.WriteBits(Speaker, 32); State.Serialize(aWriter);
}
void RequestDialogueListen::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    uint64_t value{};
    m_valid = aReader.ReadBits(value, 1); Query = value != 0;
    m_valid = m_valid && aReader.ReadBits(value, 32); Speaker = static_cast<uint32_t>(value);
    m_valid = m_valid && State.Deserialize(aReader);
}
