
#include <BSGraphics/BSGraphicsRenderer.h>
#include <Services/OverlayService.h>
#include <World.h>

struct BSInputDeviceManager;

void (*BSInputDeviceManager_PollInputDevices)(BSInputDeviceManager*, float) = nullptr;

void Hook_BSInputDeviceManager_PollInputDevices(BSInputDeviceManager* inputDeviceMgr, float afDelta)
{
    // The modal CEF overlay owns all input while active. Its controller bridge
    // polls XInput independently, so allowing Skyrim to poll here would send
    // the same D-pad/A press to the native menu underneath the options page.
    static auto s_resumeGameInputAt = std::chrono::steady_clock::time_point{};
    const bool overlayActive = World::Get().GetOverlayService().GetActive();
    if (overlayActive)
        s_resumeGameInputAt = std::chrono::steady_clock::now() + 250ms;

    if (!BSGraphics::GetMainWindow()->IsForeground() || overlayActive ||
        std::chrono::steady_clock::now() < s_resumeGameInputAt)
        return;

    BSInputDeviceManager_PollInputDevices(inputDeviceMgr, afDelta);
}

static TiltedPhoques::Initializer s_initInputDeviceManager(
    []()
    {
        const VersionDbPtr<void> pollInputDevices(68617);

        BSInputDeviceManager_PollInputDevices = static_cast<decltype(BSInputDeviceManager_PollInputDevices)>(pollInputDevices.GetPtr());

        TP_HOOK_IMMEDIATE(&BSInputDeviceManager_PollInputDevices, &Hook_BSInputDeviceManager_PollInputDevices);
    });
