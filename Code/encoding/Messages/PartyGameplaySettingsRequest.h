#pragma once

#include "Message.h"

// Host-owned live gameplay preferences. Death policy remains separate until
// its downed/revive state machine is implemented.
struct PartyGameplaySettingsRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kPartyGameplaySettingsRequest;

    PartyGameplaySettingsRequest() : ClientMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    uint32_t Difficulty{};
    bool PvpEnabled{};
};
