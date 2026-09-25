#pragma once

#include "Message.h"

// Server -> every party member (leader included): write the checkpoint save for this ID now.
struct NotifyCheckpointSave final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyCheckpointSave;
    NotifyCheckpointSave() : ServerMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const NotifyCheckpointSave& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && CheckpointId == acRhs.CheckpointId && AuthorityEpoch == acRhs.AuthorityEpoch;
    }

    TiltedPhoques::String CheckpointId{};
    uint64_t AuthorityEpoch{};
};
