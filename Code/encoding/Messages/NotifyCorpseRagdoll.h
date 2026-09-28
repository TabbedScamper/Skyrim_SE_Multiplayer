#pragma once

#include "Message.h"
#include <Messages/CorpseRagdollRequest.h>

// Server -> the other party members: the owner's ragdoll or detached part.
struct NotifyCorpseRagdoll final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyCorpseRagdoll;
    NotifyCorpseRagdoll() : ServerMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const NotifyCorpseRagdoll& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && Tick == acRhs.Tick && Bodies == acRhs.Bodies &&
               Limb == acRhs.Limb && DismemberTick == acRhs.DismemberTick && Settled == acRhs.Settled && Active == acRhs.Active && Dying == acRhs.Dying &&
               std::equal(std::begin(Origin), std::end(Origin), std::begin(acRhs.Origin)) && Heading == acRhs.Heading;
    }

    uint32_t ServerId{};
    uint64_t Tick{};
    uint32_t Limb{};
    uint64_t DismemberTick{};
    bool Settled{};
    bool Active{true};
    bool Dying{};
    // The owner's actor position; body positions are offsets from it. Placing them from the
    // receiver's own actor position followed that copy wherever its local ragdoll dragged it.
    float Origin[3]{};
    // The owner's actor heading (reference rotation z, radians). The game draws a ragdolled actor relative to its
    // reference, and the owner turns it at death; a copy left at its old heading showed the body spinning.
    float Heading{};
    TiltedPhoques::Vector<CorpseRagdollBody> Bodies{};
};
