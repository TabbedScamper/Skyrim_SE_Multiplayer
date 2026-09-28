#pragma once
#include <Messages/WorldState.h>

struct RequestWorldState final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestWorldState;
    RequestWorldState() : ClientMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { State.Serialize(aWriter); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override { m_valid = State.Deserialize(aReader); }
    bool operator==(const RequestWorldState& aOther) const noexcept { return State == aOther.State; }
    WorldState State{};
};
