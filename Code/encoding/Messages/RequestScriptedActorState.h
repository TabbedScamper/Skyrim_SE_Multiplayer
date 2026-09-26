#pragma once

#include <Messages/Message.h>
#include <Messages/ScriptedActorState.h>

struct RequestScriptedActorState final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestScriptedActorState;
    RequestScriptedActorState() : ClientMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const RequestScriptedActorState& acRhs) const noexcept { return State == acRhs.State && Anchor == acRhs.Anchor; }

    ScriptedActorState State{};
    ScriptedActorState Anchor{};
};
