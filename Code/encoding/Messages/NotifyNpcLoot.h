#pragma once
#include <Messages/NpcInventory.h>

struct NotifyNpcLoot final : ServerMessage, NpcLootData
{
    using ServerMessage::Serialize;
    static constexpr ServerOpcode Opcode = kNotifyNpcLoot;
    NotifyNpcLoot() : ServerMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override { NpcLootData::Serialize(w); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override { m_valid = NpcLootData::Deserialize(r); }
};
