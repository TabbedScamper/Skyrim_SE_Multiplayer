#pragma once
#include <Messages/NpcInventory.h>

struct RequestNpcWorn final : ClientMessage, NpcWornData
{
    using ClientMessage::Serialize;
    static constexpr ClientOpcode Opcode = kRequestNpcWorn;
    RequestNpcWorn() : ClientMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override { NpcWornData::Serialize(w); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override { m_valid = NpcWornData::Deserialize(r); }
};
