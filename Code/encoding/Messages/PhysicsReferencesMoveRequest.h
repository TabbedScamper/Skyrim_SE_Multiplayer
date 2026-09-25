#pragma once

#include "Message.h"
#include <Structs/PhysicsReferenceUpdate.h>

struct PhysicsReferencesMoveRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kPhysicsReferencesMoveRequest;
    PhysicsReferencesMoveRequest() : ClientMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    uint64_t Tick{};
    TiltedPhoques::Vector<PhysicsReferenceUpdate> Updates{};
};
