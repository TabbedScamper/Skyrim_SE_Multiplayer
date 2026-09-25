#pragma once

#include "Message.h"
#include <Structs/SceneTimelineSnapshot.h>

struct NotifySceneTimeline final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifySceneTimeline;
    NotifySceneTimeline() : ServerMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const NotifySceneTimeline& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Snapshot == acRhs.Snapshot;
    }

    SceneTimelineSnapshot Snapshot{};
};
