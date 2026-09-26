#pragma once

#include "Message.h"
#include "BusyLockData.h"

struct NotifyBusyLock final : ServerMessage, BusyLockData
{
    static constexpr ServerOpcode Opcode = kNotifyBusyLock;
    NotifyBusyLock() : ServerMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const NotifyBusyLock& aOther) const noexcept { return BusyLockData::operator==(aOther); }
};
