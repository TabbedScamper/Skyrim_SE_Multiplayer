#pragma once

#include <Structs/GameId.h>
#include <array>

// Camera policy and native GetRotation output, never a camera-root world matrix.
struct ScriptedCameraState
{
    static constexpr uint8_t kCameraControls = 0x22; // ControlMap: looking, POV switch
    uint64_t Epoch{};
    uint64_t Sequence{};
    bool Active{};
    bool FreeLook{};
    bool FreePov{};
    uint8_t Controls{};
    uint8_t StateId{};
    uint8_t Walking{}; // 0 unknown (loaded save), 1 stopped, 2 playing
    GameId WalkingStart{};
    GameId WalkingEnd{};
    std::array<float, 4> Rotation{1.f, 0.f, 0.f, 0.f}; // native NiQuaternion: w,x,y,z
    float Pitch{};
    float Heading{};

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    bool Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
    bool IsValid() const noexcept;
    bool operator==(const ScriptedCameraState&) const noexcept = default;
};
