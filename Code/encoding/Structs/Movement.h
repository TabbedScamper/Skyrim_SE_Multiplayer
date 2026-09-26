#pragma once

#include <Structs/GameId.h>

#include <Structs/Vector3_NetQuantize.h>
#include <Structs/Rotator2_NetQuantize.h>
#include <Structs/AnimationVariables.h>

using TiltedPhoques::Buffer;

struct Movement
{
    Movement() = default;
    ~Movement() = default;

    bool operator==(const Movement& acRhs) const noexcept;
    bool operator!=(const Movement& acRhs) const noexcept;

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    GameId CellId{};
    GameId WorldSpaceId{};
    Vector3_NetQuantize Position{};
    Rotator2_NetQuantize Rotation{};
    AnimationVariables Variables{};
    float Direction{};
    // Optional player camera look: yaw in low 16 bits [0, 2*pi], pitch in
    // high 16 bits [-pi/2, pi/2]. NPC updates carry only the presence bit.
    bool HasLookDirection{};
    uint32_t LookDirection{};
};
