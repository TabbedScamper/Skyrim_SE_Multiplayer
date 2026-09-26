#pragma once
#include "Message.h"
#include "PlayerControlState.h"

struct RequestPlayerControlState final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestPlayerControlState;
    RequestPlayerControlState() : ClientMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const RequestPlayerControlState& acRhs) const noexcept { return State == acRhs.State; }
    PlayerControlState State{};
};
