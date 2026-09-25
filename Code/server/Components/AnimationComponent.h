#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Game/Animation/ActionReplayCache.h>
#include <Structs/ActionEvent.h>
#include <Structs/EvaluatedPoseSnapshot.h>
#include <Structs/VisualBoneSnapshot.h>

struct AnimationComponent
{
    Vector<ActionEvent> Actions;
    ActionEvent CurrentAction;
    ActionReplayCache ActionsReplayCache;
    EvaluatedPoseSnapshot EvaluatedPose;
    bool EvaluatedPosePending{};
    VisualBoneSnapshot VisualBones;
    bool VisualBonesPending{};
};
