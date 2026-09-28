#pragma once
#include <Messages/NpcInventory.h>

struct NotifyNpcWorn final : ServerMessage, NpcWornData
{
    using ServerMessage::Serialize;
    static constexpr ServerOpcode Opcode = kNotifyNpcWorn;
    NotifyNpcWorn() : ServerMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override { NpcWornData::Serialize(w); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override { m_valid = NpcWornData::Deserialize(r); }
};
