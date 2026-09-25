#pragma once

#include "Message.h"
#include <Messages/CorpseRagdollRequest.h>

// Server -> the other party members: the owner's settled ragdoll for this corpse.
struct NotifyCorpseRagdoll final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyCorpseRagdoll;
    NotifyCorpseRagdoll() : ServerMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const NotifyCorpseRagdoll& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && Tick == acRhs.Tick && Bodies == acRhs.Bodies;
    }

    uint32_t ServerId{};
    uint64_t Tick{};
    TiltedPhoques::Vector<CorpseRagdollBody> Bodies{};
};
