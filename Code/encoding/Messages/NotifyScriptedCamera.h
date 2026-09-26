#pragma once
#include "Message.h"
#include "ScriptedCameraState.h"

struct NotifyScriptedCamera final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyScriptedCamera;
    NotifyScriptedCamera() : ServerMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const NotifyScriptedCamera& acRhs) const noexcept { return LeaderId == acRhs.LeaderId && State == acRhs.State; }
    uint32_t LeaderId{};
    ScriptedCameraState State{};
};
