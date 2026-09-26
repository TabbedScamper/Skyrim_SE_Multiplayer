#include <Messages/NotifyPlayerControlState.h>

void NotifyPlayerControlState::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(LeaderId, 32);
    State.Serialize(aWriter);
}

void NotifyPlayerControlState::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    uint64_t value{};
    m_valid = aReader.ReadBits(value, 32);
    LeaderId = static_cast<uint32_t>(value);
    m_valid = m_valid && LeaderId && State.Deserialize(aReader);
}
