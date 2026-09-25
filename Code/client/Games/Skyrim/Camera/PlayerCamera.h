#pragma once

#include <Camera/TESCamera.h>

struct PlayerCamera : public TESCamera
{
    static PlayerCamera* Get() noexcept;

    bool IsFirstPerson() noexcept;

    bool WorldPtToScreenPt3(const NiPoint3& in, NiPoint3& out, float zeroTolerance = 1e-5f);

    void ForceFirstPerson() noexcept;
    void ForceThirdPerson() noexcept;

    [[nodiscard]] TESCameraState* GetStateById(uint8_t aStateId) const noexcept;
    [[nodiscard]] float GetWorldFov() const noexcept;
    bool SetWorldFov(float aFov) noexcept;

    // TESCamera owns the camera transform, root node and current state.
    // Do not redeclare them here: doing so shifts every PlayerCamera access to
    // a duplicate block that does not exist in Skyrim's native layout.
};
