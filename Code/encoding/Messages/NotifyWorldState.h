#pragma once
#include <Messages/WorldState.h>

struct NotifyWorldState final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyWorldState;
    NotifyWorldState() : ServerMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override
    {
        aWriter.WriteBits(LeaderId, 32);
        State.Serialize(aWriter);
    }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        uint64_t value{};
        m_valid = aReader.ReadBits(value, 32);
        LeaderId = static_cast<uint32_t>(value);
        m_valid = m_valid && State.Deserialize(aReader);
    }
    bool operator==(const NotifyWorldState& aOther) const noexcept { return LeaderId == aOther.LeaderId && State == aOther.State; }
    uint32_t LeaderId{};
    WorldState State{};
};
