#include <Messages/NotifyPartyUnstuck.h>
#include <TiltedCore/Serialization.hpp>

void NotifyPartyUnstuck::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Move.Serialize(aWriter);
    aWriter.WriteBits(LeaderId, 32);
    aWriter.WriteBits(Slot, 32);
    State.Serialize(aWriter);
}

void NotifyPartyUnstuck::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    m_valid = false;
    if (!Move.Deserialize(aReader))
        return;
    uint64_t value{};
    if (!aReader.ReadBits(value, 32))
        return;
    LeaderId = static_cast<uint32_t>(value);
    if (!aReader.ReadBits(value, 32))
        return;
    Slot = static_cast<uint32_t>(value);
    m_valid = State.Deserialize(aReader) && State.IsValid(Move);
}
