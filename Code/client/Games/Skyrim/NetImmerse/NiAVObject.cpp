#include <NetImmerse/NiAVObject.h>
#include <TiltedOnlinePCH.h>

bool NiAVObject::SetMotionType(uint32_t aMotionType, bool aArg2, bool aArg3, bool aAllowActivate) noexcept
{
    TP_THIS_FUNCTION(TSetMotionType, bool, NiAVObject, uint32_t, bool, bool, bool);
    // Skyrim AE/1.7.x Address Library ID. 76033 is the SE ID and resolves to
    // unrelated code on the runtime supported by this build.
    POINTER_SKYRIMSE(TSetMotionType, s_setMotionType, 77866);
    return TiltedPhoques::ThisCall(s_setMotionType, this, aMotionType, aArg2, aArg3, aAllowActivate);
}
