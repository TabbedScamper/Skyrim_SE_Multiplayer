#pragma once

#include "Message.h"

struct NotifyHealthChangeBroadcast final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyHealthChangeBroadcast;

    NotifyHealthChangeBroadcast()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyHealthChangeBroadcast& acRhs) const noexcept { return Id == acRhs.Id && DeltaHealth == acRhs.DeltaHealth && KillerId == acRhs.KillerId && GetOpcode() == acRhs.GetOpcode(); }

    uint32_t Id;
    float DeltaHealth;
    uint32_t KillerId{};
};
