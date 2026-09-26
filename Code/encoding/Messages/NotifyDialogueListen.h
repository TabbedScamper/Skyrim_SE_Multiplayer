#pragma once
#include "Message.h"
#include "DialogueListenState.h"

struct NotifyDialogueListen final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyDialogueListen;
    NotifyDialogueListen() : ServerMessage(Opcode) {}
    uint32_t Speaker{}; // Set by the server, never accepted from a publishing client.
    DialogueListenState State;
    void SerializeRaw(TiltedPhoques::Buffer::Writer&) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader&) noexcept override;
    bool operator==(const NotifyDialogueListen& aOther) const noexcept
    { return Speaker == aOther.Speaker && State == aOther.State; }
};
