#pragma once

#include "Message.h"
#include <Structs/CameraStateSnapshot.h>

struct CameraStateRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kCameraStateRequest;
    CameraStateRequest() : ClientMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const CameraStateRequest& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Snapshot == acRhs.Snapshot;
    }

    CameraStateSnapshot Snapshot{};
};
