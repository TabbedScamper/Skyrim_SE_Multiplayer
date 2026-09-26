#pragma once

#include "Message.h"
#include "DoorVoteData.h"

struct NotifyDoorVote final : ServerMessage, DoorVoteData
{
    static constexpr ServerOpcode Opcode = kNotifyDoorVote;
    NotifyDoorVote() : ServerMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const NotifyDoorVote& aOther) const noexcept { return DoorVoteData::operator==(aOther); }
};
