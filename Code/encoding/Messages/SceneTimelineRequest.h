#pragma once

#include "Message.h"
#include <Structs/SceneTimelineSnapshot.h>

struct SceneTimelineRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kSceneTimelineRequest;
    SceneTimelineRequest() : ClientMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const SceneTimelineRequest& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Snapshot == acRhs.Snapshot;
    }

    SceneTimelineSnapshot Snapshot{};
};
