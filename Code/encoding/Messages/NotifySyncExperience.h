#pragma once

#include "Message.h"

struct NotifySyncExperience final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifySyncExperience;

    NotifySyncExperience()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifySyncExperience& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Skill == acRhs.Skill && Experience == acRhs.Experience; }

    uint8_t Skill{};
    float Experience{};
};
