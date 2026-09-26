#include <Messages/NotifyScriptedCamera.h>
#include <Structs/CheckedRead.h>

void NotifyScriptedCamera::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(LeaderId, 32);
    State.Serialize(aWriter);
}

void NotifyScriptedCamera::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    uint64_t value{};
    m_valid = aReader.ReadBits(value, 32);
    LeaderId = static_cast<uint32_t>(value);
    m_valid = m_valid && State.Deserialize(aReader);
}
