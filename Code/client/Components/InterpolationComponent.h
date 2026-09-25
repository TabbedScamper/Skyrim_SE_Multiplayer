#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/AnimationVariables.h>
#include <Structs/GameId.h>

struct InterpolationComponent
{
    struct TimePoint
    {
        uint64_t Tick{};
        glm::vec3 Position{};
        glm::vec3 Rotation{};
        AnimationVariables Variables{};
        float Direction{};

        TimePoint() = default;
        TimePoint(const TimePoint&) = default;
        TimePoint& operator=(const TimePoint&) = default;
    };

    List<TimePoint> TimePoints;
    glm::vec3 Position;
    GameId AuthorityCellId{};
    GameId AuthorityWorldSpaceId{};
    glm::vec3 AuthorityPosition{};
    uint64_t AuthorityTick{};
    uint64_t AuthorityStableSinceTick{};
    uint64_t LastCorpseCorrectionTick{};
    glm::vec3 LastCorpseCorrectionPosition{};
    uint32_t LastCorpseCorrectionCellId{};
    uint32_t CorpseCorrectionAttempts{};
    uint32_t CorpseCorrectionAttemptsForTarget{};
};
