#pragma once

#include "Message.h"
#include <Structs/GameId.h>

// A player grabbed (Hold) or let go of and saw settle (!Hold) a loose world object: it asks to stream that object's
// physics itself while it carries it, instead of the leader.
struct PhysicsLeaseRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kPhysicsLeaseRequest;
    PhysicsLeaseRequest() : ClientMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const PhysicsLeaseRequest& acRhs) const noexcept
    {
        return Id == acRhs.Id && Hold == acRhs.Hold && Epoch == acRhs.Epoch;
    }

    GameId Id{};
    bool Hold{};
    uint64_t Epoch{};
};
