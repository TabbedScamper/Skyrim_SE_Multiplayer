#pragma once

#include "Message.h"
#include "DoorVoteData.h"

struct DoorVoteRequest final : ClientMessage, DoorVoteData
{
    static constexpr ClientOpcode Opcode = kDoorVoteRequest;
    DoorVoteRequest() : ClientMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const DoorVoteRequest& aOther) const noexcept { return DoorVoteData::operator==(aOther); }
};
