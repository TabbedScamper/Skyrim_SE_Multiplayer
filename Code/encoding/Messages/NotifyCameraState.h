#pragma once

#include "Message.h"
#include <Structs/CameraStateSnapshot.h>

struct NotifyCameraState final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyCameraState;
    NotifyCameraState() : ServerMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyCameraState& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Snapshot == acRhs.Snapshot;
    }

    CameraStateSnapshot Snapshot{};
};
