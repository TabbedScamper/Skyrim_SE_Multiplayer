
#include <Camera/TESCamera.h>
#include <Camera/TESCameraState.h>
#include <NetImmerse/NiNode.h>
#include <TiltedOnlinePCH.h>

NiCamera* TESCamera::GetNiCamera()
{
    POINTER_SKYRIMSE(NiRTTI, NiCameraRTTI, 410506);
    // usually the first child should be the camera
    for (auto* child : cameraNode->children)
    {
        if (child && child->GetRTTI() == NiCameraRTTI.Get())
            return reinterpret_cast<NiCamera*>(child);
    }

    return nullptr;
}

bool TESCamera::SetState(TESCameraState* apState) noexcept
{
    if (!apState || apState->id >= 13)
        return false;

    TP_THIS_FUNCTION(TSetState, void, TESCamera, TESCameraState*);
    // CommonLibSSE-NG Offset::TESCamera::SetState AE ID. The supported
    // 1.7.104 Address Library resolves this ID; never call a raw address.
    POINTER_SKYRIMSE(TSetState, s_setState, 33026);
    TiltedPhoques::ThisCall(s_setState, this, apState);
    return true;
}
