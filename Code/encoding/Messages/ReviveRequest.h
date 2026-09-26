#pragma once

#include "Message.h"
#include "ReviveData.h"

struct ReviveRequest final : ClientMessage, ReviveData
{
    static constexpr ClientOpcode Opcode = kReviveRequest;
    ReviveRequest() : ClientMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const ReviveRequest& aOther) const noexcept { return ReviveData::operator==(aOther); }
};
