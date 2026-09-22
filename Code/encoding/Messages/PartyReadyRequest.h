#pragma once

#include "Message.h"

struct PartyReadyRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kPartyReadyRequest;

    PartyReadyRequest()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool Ready{};
};
