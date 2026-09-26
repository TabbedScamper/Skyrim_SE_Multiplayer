#pragma once

#include "Message.h"
#include "ReviveData.h"

struct NotifyRevive final : ServerMessage, ReviveData
{
    static constexpr ServerOpcode Opcode = kNotifyRevive;
    NotifyRevive() : ServerMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const NotifyRevive& aOther) const noexcept { return ReviveData::operator==(aOther); }
};
