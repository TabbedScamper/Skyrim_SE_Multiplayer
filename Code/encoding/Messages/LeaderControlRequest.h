#pragma once

#include "Message.h"

// Party leader -> server: whether the leader has free control of its character (no intro or
// cutscene holding it). Players pass through each other until it does (PlayerCollision).
struct LeaderControlRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kLeaderControlRequest;
    LeaderControlRequest() : ClientMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const LeaderControlRequest& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && FreeControl == acRhs.FreeControl; }

    bool FreeControl{};
};
