#pragma once

#include "Message.h"
#include <Structs/GameId.h>

// Who streams a loose world object's physics: HolderId is the carrying player, or 0 when it is the leader's again.
struct NotifyPhysicsLease final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPhysicsLease;
    NotifyPhysicsLease() : ServerMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const NotifyPhysicsLease& acRhs) const noexcept
    {
        return Id == acRhs.Id && HolderId == acRhs.HolderId && Epoch == acRhs.Epoch;
    }

    GameId Id{};
    uint32_t HolderId{};
    uint64_t Epoch{};
};
