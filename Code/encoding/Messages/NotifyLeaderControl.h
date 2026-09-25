#pragma once

#include "Message.h"

// Server -> the other party members: the leader's free control (see LeaderControlRequest).
struct NotifyLeaderControl final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyLeaderControl;
    NotifyLeaderControl() : ServerMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const NotifyLeaderControl& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && FreeControl == acRhs.FreeControl; }

    bool FreeControl{};
};
