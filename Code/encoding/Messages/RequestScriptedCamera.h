#pragma once
#include "Message.h"
#include "ScriptedCameraState.h"

struct RequestScriptedCamera final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestScriptedCamera;
    RequestScriptedCamera() : ClientMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const RequestScriptedCamera& acRhs) const noexcept { return State == acRhs.State; }
    ScriptedCameraState State{};
};
