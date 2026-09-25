#pragma once

#include <Structs/Movement.h>
#include <Structs/ActionEvent.h>
#include <Structs/EvaluatedPoseSnapshot.h>
#include <Structs/VisualBoneSnapshot.h>

using TiltedPhoques::Buffer;
using TiltedPhoques::Vector;

struct ReferenceUpdate
{
    ReferenceUpdate() = default;
    ~ReferenceUpdate() = default;

    bool operator==(const ReferenceUpdate& acRhs) const noexcept;
    bool operator!=(const ReferenceUpdate& acRhs) const noexcept;

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader);

    Movement UpdatedMovement{};
    // Owner's selected combat target by network actor ID. Zero means none;
    // UINT32_MAX means the native target has no network identity yet.
    uint32_t CombatTargetServerId{0xFFFFFFFFu};
    Vector<ActionEvent> ActionEvents{};
    EvaluatedPoseSnapshot EvaluatedPose{};
    VisualBoneSnapshot VisualBones{};
};
