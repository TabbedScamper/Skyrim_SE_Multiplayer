#pragma once

#include "Message.h"

struct PartyStartRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kPartyStartRequest;

    enum CampaignMode : uint8_t
    {
        kUnset,
        kNew,
        kContinue
    };

    PartyStartRequest()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    uint8_t Mode{kUnset};
    bool Launch{};
    TiltedPhoques::String CheckpointId{};
};
