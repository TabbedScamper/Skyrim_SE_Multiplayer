#pragma once

#include <Structs/GameId.h>
#include <array>
#include <glm/geometric.hpp>

struct PhysicsReferenceUpdate
{
    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    GameId Id{};
    glm::vec3 Position{};
    glm::vec3 Rotation{};
    // 0: passive reference stream; 3: dynamic Havok body (native motion type).
    uint8_t MotionType{};
    glm::vec3 LinearVelocity{};
    // Host Havok transform in native units. A reference/node transform alone
    // cannot reproduce a constrained body or its body-to-node pivot.
    std::array<float, 16> BodyTransform{};
};

// A dynamic Havok body can move or spin before TESObjectREFR::position changes.
// Compare against the last *sent* body state so small steps accumulate rather
// than disappearing below the per-scan threshold.
inline bool PhysicsBodyMotionChanged(const std::array<float, 16>& acPrevious,
    const glm::vec3& acPreviousVelocity, const std::array<float, 16>& acCurrent,
    const glm::vec3& acCurrentVelocity) noexcept
{
    float translationErrorSquared = 0.f;
    float rotationErrorSquared = 0.f;
    for (size_t axis = 0; axis < 3; ++axis)
    {
        const auto difference = acCurrent[12 + axis] - acPrevious[12 + axis];
        translationErrorSquared += difference * difference;
    }
    for (size_t row = 0; row < 3; ++row)
        for (size_t column = 0; column < 3; ++column)
        {
            const auto index = row * 4 + column;
            const auto difference = acCurrent[index] - acPrevious[index];
            rotationErrorSquared += difference * difference;
        }
    const auto velocityDelta = acCurrentVelocity - acPreviousVelocity;
    return translationErrorSquared >= 0.015f * 0.015f ||
        rotationErrorSquared >= 0.0004f ||
        glm::dot(velocityDelta, velocityDelta) >= 0.15f * 0.15f;
}
