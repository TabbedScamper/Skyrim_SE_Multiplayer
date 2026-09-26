#pragma once

#include "Message.h"
#include "BusyLockData.h"

struct BusyLockRequest final : ClientMessage, BusyLockData
{
    static constexpr ClientOpcode Opcode = kBusyLockRequest;
    BusyLockRequest() : ClientMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const BusyLockRequest& aOther) const noexcept { return BusyLockData::operator==(aOther); }
};
