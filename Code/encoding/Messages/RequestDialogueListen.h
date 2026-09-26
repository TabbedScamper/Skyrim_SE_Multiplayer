#pragma once
#include "Message.h"
#include "DialogueListenState.h"

struct RequestDialogueListen final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestDialogueListen;
    RequestDialogueListen() : ClientMessage(Opcode) {}
    // Query only requests a current snapshot. It never starts dialogue on the server.
    bool Query{};
    uint32_t Speaker{};
    DialogueListenState State;
    void SerializeRaw(TiltedPhoques::Buffer::Writer&) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader&) noexcept override;
    bool operator==(const RequestDialogueListen& aOther) const noexcept
    { return Query == aOther.Query && Speaker == aOther.Speaker && State == aOther.State; }
};
