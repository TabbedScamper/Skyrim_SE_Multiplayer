#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/AnimationVariables.h>

struct MovementComponent
{
    uint64_t Tick;
    glm::vec3 Position;
    glm::vec3 Rotation;
    AnimationVariables Variables;
    float Direction;
    // A player's camera look (Movement::LookDirection), passed through to the other players.
    bool HasLookDirection{};
    uint32_t LookDirection{};
    uint32_t CombatTargetServerId{0xFFFFFFFFu};

    bool Sent;
};
