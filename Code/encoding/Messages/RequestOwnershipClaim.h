#pragma once

#include "Message.h"

struct RequestOwnershipClaim final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestOwnershipClaim;

    RequestOwnershipClaim()
        : ClientMessage(Opcode)
    {
    }

    virtual ~RequestOwnershipClaim() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestOwnershipClaim& achRhs) const noexcept { return ServerId == achRhs.ServerId && ExpectedOwnershipEpoch == achRhs.ExpectedOwnershipEpoch && Carry == achRhs.Carry && GetOpcode() == achRhs.GetOpcode(); }

    uint32_t ServerId{};
    uint32_t ExpectedOwnershipEpoch{};
    // Hold-to-grab of a corpse: 1 = this player picked it up (it simulates the body while carrying it), 2 = it let go
    // and the body came to rest (the leader may take it back). 0 = an ordinary claim. 3 = the leader's own game has the
    // actor loaded in the leader's cell (the engine carried it there): the leader takes it whatever cell is recorded.
    uint8_t Carry{};
};
