
#include <Camera/PlayerCamera.h>
#include <Camera/TESCameraState.h>
#include <NetImmerse/NiCamera.h>
#include <TiltedOnlinePCH.h>

#include <cmath>

PlayerCamera* PlayerCamera::Get() noexcept
{
    POINTER_SKYRIMSE(PlayerCamera*, s_instance, 400802);
    return *(s_instance.Get());
}

bool PlayerCamera::IsFirstPerson() noexcept
{
    TP_THIS_FUNCTION(TIsFirstPerson, void, PlayerCamera, void*, void*, double*);
    POINTER_SKYRIMSE(TIsFirstPerson, isFirstPerson, 21600);

    double firstPerson = 0.0;
    TiltedPhoques::ThisCall(isFirstPerson, this, nullptr, nullptr, &firstPerson);

    return firstPerson == 1.0;
}

bool PlayerCamera::WorldPtToScreenPt3(const NiPoint3& in, NiPoint3& out, float zeroTolerance /* = 1e-5f */)
{
    auto* pCam = GetNiCamera();
    if (cameraNode && pCam)
    {
        return pCam->WorldPtToScreenPt3(in, out, zeroTolerance);
    }

    return false;
}

void PlayerCamera::ForceFirstPerson() noexcept
{
    TP_THIS_FUNCTION(TForceFirstPerson, void, PlayerCamera);
    POINTER_SKYRIMSE(TForceFirstPerson, forceFirstPerson, 50790);
    TiltedPhoques::ThisCall(forceFirstPerson, this);
}

void PlayerCamera::ForceThirdPerson() noexcept
{
    TP_THIS_FUNCTION(TForceThirdPerson, void, PlayerCamera);
    POINTER_SKYRIMSE(TForceThirdPerson, forceThirdPerson, 50796);
    TiltedPhoques::ThisCall(forceThirdPerson, this);
}

TESCameraState* PlayerCamera::GetStateById(const uint8_t aStateId) const noexcept
{
    if (aStateId >= 13)
        return nullptr;

    // CommonLibSSE-NG b93280e PlayerCamera::cameraStates on non-VR Skyrim.
    const auto* states = reinterpret_cast<TESCameraState* const*>(
        reinterpret_cast<const uint8_t*>(this) + 0xB8);
    auto* pState = states[aStateId];
    return pState && pState->id == aStateId ? pState : nullptr;
}

float PlayerCamera::GetWorldFov() const noexcept
{
    return *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(this) + 0x13C);
}

bool PlayerCamera::SetWorldFov(const float aFov) noexcept
{
    if (!std::isfinite(aFov) || aFov < 1.f || aFov > 179.f)
        return false;

    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(this) + 0x13C) = aFov;
    return true;
}
