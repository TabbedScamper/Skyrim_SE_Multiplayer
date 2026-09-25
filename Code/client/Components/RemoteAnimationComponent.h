#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/EvaluatedPoseSnapshot.h>
#include <Structs/VisualBoneSnapshot.h>

struct RemoteAnimationComponent
{
    struct CombatTargetPoint
    {
        uint64_t Tick{};
        uint32_t ServerId{0xFFFFFFFFu};
    };

    List<ActionEvent> TimePoints;
    List<CombatTargetPoint> CombatTargetTimePoints;
    ActionEvent LastRanAction;
    ActionEvent LastReceivedAction;
    bool LastRanActionResult{};
    uint32_t DesiredCombatTargetServerId{0xFFFFFFFFu};
    uint64_t DesiredCombatTargetTick{};
    uint64_t LastReceivedCombatTargetTick{};
    uint64_t LastCombatTargetApplyTick{};
    uint32_t LastCombatTargetApplyDesiredFormId{};
    uint32_t LastCombatTargetApplyBeforeFormId{};
    uint32_t LastCombatTargetApplyAfterFormId{};
    ActionEvent LastProcessedAction;
    uint32_t ReplayCount;
    bool ResetAnimationGraphForReplay{false};
    EvaluatedPoseSnapshot EvaluatedPose;
    uint64_t EvaluatedPoseTick{};
    VisualBoneSnapshot VisualBones;
    uint64_t VisualBonesTick{};
};
