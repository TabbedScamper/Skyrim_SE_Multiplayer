#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/Vector3_NetQuantize.h>

enum class OwnershipReleaseReason : uint8_t
{
    Relinquish,
    DeclineGrant,
    // The leader's own scripts disabled or deleted this temporary actor where the party stands: it is gone for
    // everyone, not handed to another player (session 2026-09-29: Keep Stormcloaks stayed on the follower).
    ScriptRemoved
};

struct RequestOwnershipTransfer final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestOwnershipTransfer;

    RequestOwnershipTransfer()
        : ClientMessage(Opcode)
    {
    }

    virtual ~RequestOwnershipTransfer() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestOwnershipTransfer& achRhs) const noexcept
    {
        return GetOpcode() == achRhs.GetOpcode() && ServerId == achRhs.ServerId && OwnershipEpoch == achRhs.OwnershipEpoch && Reason == achRhs.Reason && WorldSpaceId == achRhs.WorldSpaceId && CellId == achRhs.CellId && Position == achRhs.Position;
    }

    uint32_t ServerId{};
    uint32_t OwnershipEpoch{};
    OwnershipReleaseReason Reason{OwnershipReleaseReason::Relinquish};
    GameId WorldSpaceId{};
    GameId CellId{};
    Vector3_NetQuantize Position{};
};
