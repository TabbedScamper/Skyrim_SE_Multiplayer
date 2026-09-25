
#include <BSGraphics/BSGraphicsRenderer.h>
#include <Games/Skyrim/BSInput/InputPollDiagnostic.h>
#include <Services/OverlayService.h>
#include <Services/InputService.h>
#include <World.h>

struct BSInputDeviceManager;

void (*BSInputDeviceManager_PollInputDevices)(BSInputDeviceManager*, float) = nullptr;

void Hook_BSInputDeviceManager_PollInputDevices(BSInputDeviceManager* inputDeviceMgr, float afDelta)
{
    g_inputPollDiagnostic.Calls.fetch_add(1, std::memory_order_relaxed);
    // The modal CEF overlay owns all input while active. Its controller bridge
    // polls XInput independently, so allowing Skyrim to poll here would send
    // the same D-pad/A press to the native menu underneath the options page.
    static auto s_resumeGameInputAt = std::chrono::steady_clock::time_point{};
    const bool overlayActive = World::Get().GetOverlayService().GetActive();
    // The Windows key hands the pointer to the desktop: freeze game input
    // (camera, movement, keys) exactly where it is. The resume delay keeps the
    // click that returns to the game from also landing as an attack.
    const bool handedToShell = InputService::IsPointerHandedToShell();
    if (overlayActive || handedToShell)
        s_resumeGameInputAt = std::chrono::steady_clock::now() + 250ms;

    if (!BSGraphics::GetMainWindow()->IsForeground() || handedToShell)
    {
        g_inputPollDiagnostic.Unfocused.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (overlayActive)
    {
        g_inputPollDiagnostic.OverlayActive.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (std::chrono::steady_clock::now() < s_resumeGameInputAt)
    {
        g_inputPollDiagnostic.ResumeDelay.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    g_inputPollDiagnostic.Forwarded.fetch_add(1, std::memory_order_relaxed);
    BSInputDeviceManager_PollInputDevices(inputDeviceMgr, afDelta);
}

static TiltedPhoques::Initializer s_initInputDeviceManager(
    []()
    {
        const VersionDbPtr<void> pollInputDevices(68617);

        BSInputDeviceManager_PollInputDevices = static_cast<decltype(BSInputDeviceManager_PollInputDevices)>(pollInputDevices.GetPtr());

        TP_HOOK_IMMEDIATE(&BSInputDeviceManager_PollInputDevices, &Hook_BSInputDeviceManager_PollInputDevices);
    });
