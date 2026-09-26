#pragma once
#include "Message.h"
#include "PlayerCombatState.h"

struct NotifyPlayerCombatState final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPlayerCombatState;
    NotifyPlayerCombatState() : ServerMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { State.Serialize(aWriter); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ServerMessage::DeserializeRaw(aReader);
        m_valid = State.Deserialize(aReader);
    }
    bool operator==(const NotifyPlayerCombatState& aOther) const noexcept { return State == aOther.State; }
    PlayerCombatState State;
};
