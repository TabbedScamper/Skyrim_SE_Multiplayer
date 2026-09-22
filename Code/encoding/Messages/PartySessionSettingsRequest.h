#pragma once

#include "Message.h"

using TiltedPhoques::String;

struct PartySessionSettingsRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kPartySessionSettingsRequest;

    PartySessionSettingsRequest()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool Open{};
    String Password{};
};
