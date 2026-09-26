#pragma once

#include <Messages/Message.h>
#include <Messages/ScriptedActorState.h>

struct NotifyScriptedActorState final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyScriptedActorState;
    NotifyScriptedActorState() : ServerMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const NotifyScriptedActorState& acRhs) const noexcept
    {
        return State == acRhs.State && FormId == acRhs.FormId && LeaderPlayerId == acRhs.LeaderPlayerId;
    }

    ScriptedActorState State{};
    GameId FormId{};
    uint32_t LeaderPlayerId{};
};
