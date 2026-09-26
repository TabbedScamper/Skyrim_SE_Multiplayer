#pragma once
#include "Message.h"
#include "PlayerControlState.h"

struct NotifyPlayerControlState final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPlayerControlState;
    NotifyPlayerControlState() : ServerMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const NotifyPlayerControlState& acRhs) const noexcept { return LeaderId == acRhs.LeaderId && State == acRhs.State; }
    uint32_t LeaderId{};
    PlayerControlState State{};
};
