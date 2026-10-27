
#include "AI/Movement/PlayerControls.h"
#include <atomic>

namespace { std::atomic_bool s_releaseQueued{}; }

void PlayerControls::QueueInputRelease() noexcept
{
    s_releaseQueued.store(true, std::memory_order_release);
}

void PlayerControls::DrainInputRelease() noexcept
{
    if (s_releaseQueued.exchange(false, std::memory_order_acq_rel))
    {
        if (auto* controls = GetInstance())
            controls->MarkInputRelease();
    }
}

PlayerControls* PlayerControls::GetInstance()
{
    POINTER_SKYRIMSE(PlayerControls*, s_instance, 400864);
    return *(s_instance.Get());
}

void PlayerControls::SetEnabled(bool abFlag)
{
    TP_THIS_FUNCTION(TSetEnabled, void, PlayerControls, bool);
    POINTER_SKYRIMSE(TSetEnabled, setEnabled, 42345);
    TiltedPhoques::ThisCall(setEnabled, this, abFlag);
}

void PlayerControls::SetCamSwitch(bool aSet) noexcept
{
    Data.remapMode = aSet;
}

BSInputEnableManager* BSInputEnableManager::Get()
{
    POINTER_SKYRIMSE(BSInputEnableManager*, s_instance, 400863);
    return *(s_instance.Get());
}

void BSInputEnableManager::EnableOtherEvent(int32_t aFlags, bool aEnable, bool aUnk2)
{
    TP_THIS_FUNCTION(TSetEnabled, void, BSInputEnableManager, int32_t, bool, bool);
    POINTER_SKYRIMSE(TSetEnabled, setEnabled, 68545);
    TiltedPhoques::ThisCall(setEnabled, this, aFlags, aEnable, aUnk2);
}

// static TiltedPhoques::Initializer s_Init([]() { TiltedPhoques::Put<uint8_t>(0x14072E490, 0xCC); });
