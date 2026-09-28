#pragma once

// Engine-free counterparts of currently owned implementation sites. No runtime
// hooks use these copies yet. See Tools/Replay/README.md FOLLOW-UP and provenance.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <glm/glm.hpp>

namespace ReplaySync
{
constexpr float HavokToGame = 70.f;

// InterpolationSystem::Update: retain the unsigned-tick guards and 150 ms cap.
inline float ActorFraction(uint64_t aFirst, uint64_t aSecond, uint64_t aTick) noexcept
{
    float delta = 0.f;
    if (aTick >= aSecond)
        delta = 1.f;
    else if (aTick > aFirst && aSecond > aFirst)
        delta = static_cast<float>(aTick - aFirst) / static_cast<float>(aSecond - aFirst);
    return delta;
}

inline glm::vec3 ActorPosition(glm::vec3 aFirst, glm::vec3 aSecond,
    uint64_t aFirstTick, uint64_t aSecondTick, uint64_t aTick, bool aVehicle) noexcept
{
    const float delta = ActorFraction(aFirstTick, aSecondTick, aTick);
    auto position = aFirst + (aSecond - aFirst) * delta;
    if (aVehicle && aTick > aSecondTick && aSecondTick > aFirstTick)
    {
        const float prediction = static_cast<float>((std::min)(uint64_t{150}, aTick - aSecondTick)) /
            static_cast<float>(aSecondTick - aFirstTick);
        position += (aSecond - aFirst) * prediction;
    }
    return position;
}

// ObjectService::RunNativeStep, non-assembly linear branch. All inputs/output
// are Havok units. This models the command, never collision/constraint response.
struct Steering
{
    glm::vec3 Position;
    glm::vec3 Velocity;
    bool Teleport;
};
inline Steering DynamicSteering(glm::vec3 aCurrent, glm::vec3 aWanted, glm::vec3 aFeed) noexcept
{
    const auto error = aWanted - aCurrent;
    const bool placed = glm::length(error) > 3.f;
    return {placed ? aWanted : aCurrent, aFeed + (placed ? glm::vec3{} : error / 0.1f), placed};
}

// Assembly positional target, before native E62478's COM-aware velocity drive.
inline glm::vec3 AssemblyGoal(glm::vec3 aCurrent, glm::vec3 aWanted, glm::vec3 aFeed, float aDt) noexcept
{
    glm::vec3 correction = aWanted - aCurrent;
    const float length = glm::length(correction);
    if (length > 10.f / HavokToGame)
        correction *= (10.f / HavokToGame) / length;
    return aCurrent + correction + aFeed * aDt;
}

inline glm::vec3 BoundAssemblyVelocity(glm::vec3 aVelocity, float aDt, bool aAngular) noexcept
{
    const float limit = aAngular ? 12.f : 25.f / HavokToGame / aDt;
    const float magnitude = glm::length(aVelocity);
    if (magnitude > limit)
        aVelocity *= limit / magnitude;
    return aVelocity;
}

// CorpseRagdollService::StepFrame::Stream::Evaluate quaternion math, xyzw.
inline bool RagdollRotation(const std::array<float, 4>& aFrom, const std::array<float, 4>& aTo,
    float aFraction, std::array<float, 4>& aOut) noexcept
{
    float dot = 0.f;
    for (int axis = 0; axis < 4; ++axis)
        dot += aFrom[axis] * aTo[axis];
    float norm = 0.f;
    for (int axis = 0; axis < 4; ++axis)
    {
        aOut[axis] = aFrom[axis] + ((dot < 0.f ? -1.f : 1.f) * aTo[axis] - aFrom[axis]) * aFraction;
        norm += aOut[axis] * aOut[axis];
    }
    if (!std::isfinite(norm) || norm < 0.0001f)
        return false;
    for (float& value : aOut)
        value /= std::sqrt(norm);
    return true;
}

inline float RagdollAngle(const std::array<float, 4>& aTarget, const std::array<float, 4>& aActual) noexcept
{
    float dot = 0.f;
    for (int axis = 0; axis < 4; ++axis)
        dot += aTarget[axis] * aActual[axis];
    return 2.f * std::acos(std::clamp(std::abs(dot), 0.f, 1.f)) * 57.29578f;
}

inline float RagdollDistance(glm::vec3 aTargetHavok, glm::vec3 aActualHavok) noexcept
{
    // MeasureBody converts each endpoint before subtraction. Keep that order.
    float error{};
    for (int axis = 0; axis < 3; ++axis)
    {
        const float actual = aActualHavok[axis] * HavokToGame;
        const float delta = aTargetHavok[axis] * HavokToGame - actual;
        error += delta * delta;
    }
    return std::sqrt(error);
}

// NakedNpcGuard::WornDeliveryCount. Delivery of stock after a render repair
// must not double the owner's inventory count.
constexpr int32_t WornDeliveryCount(int32_t delta, int64_t ownerCount, int64_t localCount) noexcept
{
    return int32_t(std::min<int64_t>(delta, std::max<int64_t>(0, ownerCount - localCount)));
}

enum class Verdict { Pass, Fail, Missing };
inline Verdict Below(double aValue, double aLimit) noexcept
{
    return !std::isfinite(aValue) ? Verdict::Missing : aValue < aLimit ? Verdict::Pass : Verdict::Fail;
}
inline double CartStepMetric(const std::array<double, 3>& aBefore, const std::array<double, 3>& aAfter) noexcept
{
    double squared = 0;
    for (size_t i = 0; i < 3; ++i)
        squared += (aAfter[i] - aBefore[i]) * (aAfter[i] - aBefore[i]);
    return std::sqrt(squared);
}
inline Verdict CartStep(double aStep) noexcept
{
    return !std::isfinite(aStep) ? Verdict::Missing : aStep <= 30.0 ? Verdict::Pass : Verdict::Fail;
}
inline Verdict BoneTarget(double aUnits, double aDegrees, bool aSettled) noexcept
{
    if (!std::isfinite(aUnits) || (aSettled && !std::isfinite(aDegrees))) return Verdict::Missing;
    return aUnits < (aSettled ? 5.0 : 30.0) && (!aSettled || aDegrees < 10.0) ? Verdict::Pass : Verdict::Fail;
}
inline Verdict CameraPitch(double aPitch, double aIntent, double aTolerance) noexcept
{
    // TRACKER gives no numeric tolerance. The caller must supply one in radians;
    // a +/-90 degree pitch alone is not evidence of wrong player intent.
    if (!std::isfinite(aPitch) || !std::isfinite(aIntent) || !std::isfinite(aTolerance) || aTolerance < 0)
        return Verdict::Missing;
    if (std::abs(aPitch) > 1.5707963267948966 + 1e-6) return Verdict::Fail;
    return std::abs(aPitch - aIntent) <= aTolerance ? Verdict::Pass : Verdict::Fail;
}
} // namespace ReplaySync
