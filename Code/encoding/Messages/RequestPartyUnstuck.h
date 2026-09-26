#pragma once

#include "Message.h"
#include "UnstuckMove.h"

struct RequestPartyUnstuck final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestPartyUnstuck;
    RequestPartyUnstuck() : ClientMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const RequestPartyUnstuck& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Move == acRhs.Move; }

    UnstuckMove Move{};
};
