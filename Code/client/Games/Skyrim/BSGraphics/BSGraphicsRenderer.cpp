
#include "Services/InputService.h"
#include "Services/GameTestService.h"
#include "Services/FarmMode.h"
#include "Systems/RenderSystemD3D11.h"

#include "World.h"
#include "Utils/MainThread.h"

#include "BSGraphics/BSGraphicsRenderer.h"
#include "BSRandom/BSRandom.h"
#include "Interface/UI.h"
#include <atomic>
#include <intrin.h>

// shared resource by launcher
extern HICON g_SharedWindowIcon;

namespace BSGraphics
{
namespace
{

static RenderSystemD3D11* g_sRs = nullptr;
static WNDPROC RealWndProc = nullptr;
static RendererWindow* g_RenderWindow = nullptr;
static RendererData* g_RendererData = nullptr;
static Renderer* g_Renderer = nullptr;
static std::atomic_bool g_deferredTestWake{};
static std::atomic_bool g_deferredSettingsWake{};
static std::atomic<uint32_t> g_deferredWindowMessages{};
static std::atomic<WPARAM> g_deferredSizeType{};
static std::atomic<DWORD> g_mainThreadId{};
static std::atomic_flag g_overlayBusy = ATOMIC_FLAG_INIT;

bool LoadInProgress() noexcept
{
    static VersionDbPtr<uint8_t> s_loadScreenMode(400336); // 0x14219E5F6
    if (s_loadScreenMode.Get() && *s_loadScreenMode.Get() != 0)
        return true;
    auto* ui = UI::Get();
    return ui && ui->GetMenuOpen(BSFixedString("Loading Menu"));
}

static constexpr char kTogetherWindowName[]{SSM_PRODUCT_NAME};

// Renderer::Init AL77226/141007B70 explicitly shows, foregrounds and focuses
// the game. Farm processes must never activate it, including at startup.
static decltype(&SetForegroundWindow) RealSetForegroundWindow = &SetForegroundWindow;
static decltype(&SetFocus) RealSetFocus = &SetFocus;
static decltype(&ShowWindow) RealShowWindow = &ShowWindow;
BOOL WINAPI FarmSetForegroundWindow(HWND) { return FALSE; }
HWND WINAPI FarmSetFocus(HWND) { return nullptr; }
BOOL WINAPI FarmShowWindow(HWND window, int command)
{
    return RealShowWindow(window, command == SW_HIDE ? SW_HIDE : SW_SHOWNOACTIVATE);
}

} // namespace
RendererWindow* GetMainWindow()
{
    return g_RenderWindow;
}

RendererData* GetRendererData()
{
    return g_RendererData;
}

Renderer* GetRenderer()
{
    return g_Renderer;
}

void Renderer::ResizeWindow(uint32_t aWindowId, uint32_t aWidth, uint32_t aHeight, bool aFullscreen, bool aBorderless)
{
    TP_THIS_FUNCTION(TResizeWindow, void, Renderer, uint32_t, uint32_t, uint32_t, bool, bool);
    POINTER_SKYRIMSE(TResizeWindow, s_resizeWindow, 77239);
    TiltedPhoques::ThisCall(s_resizeWindow.Get(), this, aWindowId, aWidth, aHeight, aFullscreen, aBorderless);
}

void Renderer::RequestWindowResize(uint32_t aWidth, uint32_t aHeight)
{
    TP_THIS_FUNCTION(TRequestWindowResize, void, Renderer, uint32_t, uint32_t);
    POINTER_SKYRIMSE(TRequestWindowResize, s_requestWindowResize, 77235);
    TiltedPhoques::ThisCall(s_requestWindowResize.Get(), this, aWidth, aHeight);
}

void Renderer::WindowSizeChanged(uint32_t aWindowId)
{
    TP_THIS_FUNCTION(TWindowSizeChanged, void, Renderer, uint32_t);
    POINTER_SKYRIMSE(TWindowSizeChanged, s_windowSizeChanged, 77238);
    if (s_windowSizeChanged.Get())
        TiltedPhoques::ThisCall(s_windowSizeChanged.Get(), this, aWindowId);
}

bool RendererWindow::IsForeground()
{
    return GetForegroundWindow() == hWnd;
}

void (*Renderer_Init)(Renderer*, BSGraphics::RendererInitOSData*, const BSGraphics::ApplicationWindowProperties*, BSGraphics::RendererInitReturn*) = nullptr;

// WNDPROC seems to be part of the renderer
LRESULT CALLBACK Hook_WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    MT::Scope windowScope(MT::Ctx::WndProc);
    if (FarmMode::Enabled() && uMsg == WM_WINDOWPOSCHANGING && lParam)
    {
        auto* placement = reinterpret_cast<WINDOWPOS*>(lParam);
        placement->x = placement->y = -16000;
        placement->flags = (placement->flags & ~SWP_NOMOVE) | SWP_NOACTIVATE;
    }
    if (FarmMode::Enabled() && uMsg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (uMsg == cGameTestWakeMessage && entt::locator<World>::has_value())
    {
        if (MT::CutEnabled(1) || LoadInProgress())
            g_deferredTestWake.store(true, std::memory_order_release);
        else
            World::Get().GetGameTestService().OnWindowThread();
        return 0;
    }

    if ((uMsg == cGameSettingsWakeMessage ||
        (uMsg == WM_TIMER && wParam == cGameSettingsTimerId)) &&
        entt::locator<World>::has_value())
    {
        if (MT::CutEnabled(1) || LoadInProgress())
            g_deferredSettingsWake.store(true, std::memory_order_release);
        else
            World::Get().GetGameSettingsService().OnMainLoop();
        return 0;
    }

    // A floor on the window size: the live lobby's camera, menus and overlay were laid out for real screens, and
    // dragging the window very small or very wide threw them off (owner 2026-10-01: "restrict funky small sizes").
    // The client area may not go below 1280x720; Windows applies it while the window is dragged.
    if (uMsg == WM_GETMINMAXINFO)
    {
        const auto result = RealWndProc(hwnd, uMsg, wParam, lParam);
        RECT minimum{0, 0, 1280, 720};
        AdjustWindowRectEx(&minimum, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
            static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));
        auto* pInfo = reinterpret_cast<MINMAXINFO*>(lParam);
        pInfo->ptMinTrackSize.x = std::max<LONG>(pInfo->ptMinTrackSize.x, minimum.right - minimum.left);
        pInfo->ptMinTrackSize.y = std::max<LONG>(pInfo->ptMinTrackSize.y, minimum.bottom - minimum.top);
        return result;
    }

    if (const auto inputResult = InputService::WndProc(hwnd, uMsg, wParam, lParam); inputResult != 0)
        return inputResult;

    const auto result = RealWndProc(hwnd, uMsg, wParam, lParam);

    if (uMsg == WM_SIZE && entt::locator<World>::has_value())
    {
        if (MT::CutEnabled(1))
        {
            g_deferredSizeType.store(wParam, std::memory_order_release);
            g_deferredWindowMessages.fetch_or(1u, std::memory_order_acq_rel);
        }
        else
            World::Get().GetGameSettingsService().OnWindowSizeChanged(wParam);
    }
    if ((uMsg == WM_ENTERSIZEMOVE || uMsg == WM_EXITSIZEMOVE || uMsg == WM_WINDOWPOSCHANGED) &&
        entt::locator<World>::has_value())
    {
        if (MT::CutEnabled(1))
        {
            const uint32_t bit = uMsg == WM_ENTERSIZEMOVE ? 2u : uMsg == WM_EXITSIZEMOVE ? 4u : 8u;
            g_deferredWindowMessages.fetch_or(bit, std::memory_order_acq_rel);
        }
        else
            World::Get().GetGameSettingsService().OnWindowPlacementChanged(uMsg);
    }

    // Skyrim may toggle ShowCursor inside its own focus handling, so the
    // pointer owner re-applies its rule after the real WndProc has finished.
    InputService::AfterGameWndProc(hwnd, uMsg);

    return result;
}

void DrainDeferredWindowWorkOnMainFrame() noexcept
{
    g_mainThreadId.store(GetCurrentThreadId(), std::memory_order_release);
    if (!g_deferredTestWake.load(std::memory_order_acquire) &&
        !g_deferredSettingsWake.load(std::memory_order_acquire) &&
        !g_deferredWindowMessages.load(std::memory_order_acquire))
        return;
    if (MT::Loading() || LoadInProgress() || !entt::locator<World>::has_value())
        return;
    if (MT::CutEnabled(1) && MT::CurrentSlot() != MT::Slot::S0b)
    {
        MT::M1WrongSlot.fetch_add(1, std::memory_order_relaxed);
        MT::Report();
    }
    const auto windowMessages = g_deferredWindowMessages.exchange(0, std::memory_order_acq_rel);
    auto& settings = World::Get().GetGameSettingsService();
    if (windowMessages & 2u) settings.OnWindowPlacementChanged(WM_ENTERSIZEMOVE);
    if (windowMessages & 1u) settings.OnWindowSizeChanged(g_deferredSizeType.load(std::memory_order_acquire));
    if (windowMessages & 4u) settings.OnWindowPlacementChanged(WM_EXITSIZEMOVE);
    if (windowMessages & 8u) settings.OnWindowPlacementChanged(WM_WINDOWPOSCHANGED);
    if (g_deferredTestWake.exchange(false, std::memory_order_acq_rel))
        World::Get().GetGameTestService().OnWindowThread();
    if (g_deferredSettingsWake.exchange(false, std::memory_order_acq_rel))
        World::Get().GetGameSettingsService().OnMainLoop();
}

void Hook_Renderer_Init(Renderer* self, BSGraphics::RendererInitOSData* aOSData, const BSGraphics::ApplicationWindowProperties* aFBData, BSGraphics::RendererInitReturn* aOut)
{
    // we feed this a shared icon as the resource directory of our former launcher data is already overwritten with the
    // game.
    aOSData->hIcon = g_SharedWindowIcon;
    // Append our window name.
    aOSData->pClassName = kTogetherWindowName;

    RealWndProc = aOSData->pWndProc;
    aOSData->pWndProc = Hook_WndProc;

    // A windowed game opens where the player last left it (Renderer::Init, ID
    // 77226, passes iX/iY straight to CreateWindowExA).
    if (aFBData && !aFBData->bFullScreen && !aFBData->bBorderlessWindow)
    {
        RECT outer{0, 0, static_cast<LONG>(aFBData->uiWidth), static_cast<LONG>(aFBData->uiHeight)};
        AdjustWindowRect(&outer, WS_OVERLAPPEDWINDOW, FALSE);
        int x = 0, y = 0;
        if (GameSettingsService::SavedWindowedOrigin(x, y, outer.right - outer.left))
        {
            auto* pProperties = const_cast<BSGraphics::ApplicationWindowProperties*>(aFBData);
            pProperties->iX = x;
            pProperties->iY = y;
        }
    }

    BSGraphics::ApplicationWindowProperties farmProperties{};
    if (FarmMode::Enabled() && aFBData)
    {
        farmProperties = *aFBData;
        farmProperties.uiWidth = 640; farmProperties.uiHeight = 360;
        farmProperties.iX = farmProperties.iY = -16000;
        farmProperties.bFullScreen = farmProperties.bBorderlessWindow = false;
        aFBData = &farmProperties;
    }
    Renderer_Init(self, aOSData, aFBData, aOut);
    // WinMain's unfocused branch reads this byte before calling Main::Update.
    static VersionDbPtr<uint8_t> alwaysActive(380768); // 0x1420B58C0
    if (alwaysActive.Get()) *alwaysActive.Get() = 1;

    g_sRs = &World::Get().ctx().at<RenderSystemD3D11>();
    // This how the game does it too
    g_RenderWindow = &self->Data.RenderWindowA[0];
    g_RendererData = &self->Data;
    g_Renderer = self;

    const BSGraphics::RendererData& renderer = self->Data;

    g_sRs->OnDeviceCreation(renderer.RenderWindowA[0].pSwapChain, renderer.pForwarder, renderer.pContext);
}

void (*StopTimer)(int) = nullptr;

// Insert us at the End
void Hook_StopTimer(int type)
{
    // SwapCall is at End+9. Its stack frame is 0x48 bytes, so End's caller
    // return address is 0x50 above this hook's return slot (ID 77246).
    const auto endReturn = *reinterpret_cast<const uintptr_t*>(
        reinterpret_cast<const uint8_t*>(_AddressOfReturnAddress()) + 0x50);
    static VersionDbPtr<uint8_t> s_binkFrame(90259); // 0x1411B2050; End returns at +0x23C
    const bool worker = GetCurrentThreadId() != g_mainThreadId.load(std::memory_order_acquire);
    const bool workerBink = worker && s_binkFrame.Get() &&
        endReturn == reinterpret_cast<uintptr_t>(s_binkFrame.Get()) + 0x23C;
    if (g_sRs && !g_overlayBusy.test_and_set(std::memory_order_acquire))
    {
        auto* renderer = g_Renderer;
        auto* renderLock = renderer ? reinterpret_cast<CRITICAL_SECTION*>(reinterpret_cast<uint8_t*>(renderer) + 0x27F0) : nullptr;
        if (renderLock && TryEnterCriticalSection(renderLock))
        {
            // End can also be called by the loading loop or synchronous Bink playback.
            // Only the main render caller may inspect UI, player state and GFx movies.
            const bool drawOnly = workerBink || worker;
            g_sRs->OnRender(drawOnly);
            LeaveCriticalSection(renderLock);
        }
        g_overlayBusy.clear(std::memory_order_release);
    }

    StopTimer(type);
}

static TiltedPhoques::Initializer s_viewportHooks(
    []()
    {
        if (FarmMode::Enabled())
        {
            TP_HOOK_IMMEDIATE(&RealSetForegroundWindow, &FarmSetForegroundWindow);
            TP_HOOK_IMMEDIATE(&RealSetFocus, &FarmSetFocus);
            TP_HOOK_IMMEDIATE(&RealShowWindow, &FarmShowWindow);
        }
        const VersionDbPtr<void> initWindowLoc(77226);
        // patch dwStyle in BSGraphics::InitWindows
        TiltedPhoques::Put(mem::pointer(initWindowLoc.GetPtr()) + 0x174 + 1, WS_OVERLAPPEDWINDOW);

        const VersionDbPtr<void> windowLoc(68781);
        // TODO: move me to input patches.
        // don't let the game steal the media keys in windowed mode
        TiltedPhoques::Put(
            mem::pointer(windowLoc.GetPtr()) + 0x55 + 2,
            /*strip DISCL_EXCLUSIVE bits and append DISCL_NONEXCLUSIVE*/ 3);

        const VersionDbPtr<void> timerLoc(77246);
        const VersionDbPtr<void> renderInit(77226);

        TiltedPhoques::SwapCall(mem::pointer(timerLoc.GetPtr()) + 9, StopTimer, &Hook_StopTimer);

        Renderer_Init = static_cast<decltype(Renderer_Init)>(renderInit.GetPtr());

        // Once we find a proper way to locate it for different versions, go back to swapcall
        // TiltedPhoques::SwapCall(mem::pointer(initLoc.GetPtr()) + 0xD1A, Renderer_Init, &Hook_Renderer_Init);
        TP_HOOK_IMMEDIATE(&Renderer_Init, &Hook_Renderer_Init);
    });
} // namespace BSGraphics
