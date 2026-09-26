#pragma once
#include "Message.h"
#include "PlayerCombatState.h"

struct RequestPlayerCombatState final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestPlayerCombatState;
    RequestPlayerCombatState() : ClientMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { State.Serialize(aWriter); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ClientMessage::DeserializeRaw(aReader);
        m_valid = State.Deserialize(aReader);
    }
    bool operator==(const RequestPlayerCombatState& aOther) const noexcept { return State == aOther.State; }
    PlayerCombatState State;
};
