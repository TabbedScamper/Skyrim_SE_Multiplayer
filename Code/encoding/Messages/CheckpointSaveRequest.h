#pragma once

#include "Message.h"

// Leader -> server: the leader's game just saved; every party member should write a matching
// checkpoint save so a later Continue loads the same world on each PC.
struct CheckpointSaveRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kCheckpointSaveRequest;
    CheckpointSaveRequest() : ClientMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const CheckpointSaveRequest& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && CheckpointId == acRhs.CheckpointId;
    }

    TiltedPhoques::String CheckpointId{};
};
