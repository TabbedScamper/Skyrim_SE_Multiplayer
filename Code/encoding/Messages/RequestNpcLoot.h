#pragma once
#include <Messages/NpcInventory.h>

struct RequestNpcLoot final : ClientMessage, NpcLootData
{
    using ClientMessage::Serialize;
    static constexpr ClientOpcode Opcode = kRequestNpcLoot;
    RequestNpcLoot() : ClientMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override { NpcLootData::Serialize(w); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override { m_valid = NpcLootData::Deserialize(r); }
};
