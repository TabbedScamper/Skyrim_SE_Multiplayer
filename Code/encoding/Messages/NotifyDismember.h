#pragma once

#include "Message.h"

// Reliable semantic event. Tick is on the same presentation timeline as body samples.
struct NotifyDismember final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyDismember;
    NotifyDismember() : ServerMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const NotifyDismember& acRhs) const noexcept
    {
        return ServerId == acRhs.ServerId && Limb == acRhs.Limb && Tick == acRhs.Tick;
    }

    uint32_t ServerId{};
    uint32_t Limb{1};
    uint64_t Tick{};
};
