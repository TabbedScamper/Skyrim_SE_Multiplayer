#pragma once

#include "Message.h"
#include <Structs/PhysicsReferenceUpdate.h>

struct NotifyPhysicsReferencesMove final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPhysicsReferencesMove;
    NotifyPhysicsReferencesMove() : ServerMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    uint64_t Tick{};
    uint64_t AuthorityEpoch{};
    TiltedPhoques::Vector<PhysicsReferenceUpdate> Updates{};
};
