
#include "Services/InputService.h"
#include "Services/GameTestService.h"
#include "Services/FarmMode.h"
#include "Systems/RenderSystemD3D11.h"

#include "World.h"

#include "BSGraphics/BSGraphicsRenderer.h"
#include "BSRandom/BSRandom.h"

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
    if (FarmMode::Enabled() && uMsg == WM_WINDOWPOSCHANGING && lParam)
    {
        auto* placement = reinterpret_cast<WINDOWPOS*>(lParam);
        placement->x = placement->y = -16000;
        placement->flags = (placement->flags & ~SWP_NOMOVE) | SWP_NOACTIVATE;
    }
    if (FarmMode::Enabled() && uMsg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (uMsg == cGameTestWakeMessage && entt::locator<World>::has_value())
    {
        World::Get().GetGameTestService().OnWindowThread();
        return 0;
    }

    if ((uMsg == cGameSettingsWakeMessage ||
        (uMsg == WM_TIMER && wParam == cGameSettingsTimerId)) &&
        entt::locator<World>::has_value())
    {
        World::Get().GetGameSettingsService().OnMainLoop();
        return 0;
    }

    if (const auto inputResult = InputService::WndProc(hwnd, uMsg, wParam, lParam); inputResult != 0)
        return inputResult;

    const auto result = RealWndProc(hwnd, uMsg, wParam, lParam);

    if (uMsg == WM_SIZE && entt::locator<World>::has_value())
        World::Get().GetGameSettingsService().OnWindowSizeChanged(wParam);
    if ((uMsg == WM_ENTERSIZEMOVE || uMsg == WM_EXITSIZEMOVE || uMsg == WM_WINDOWPOSCHANGED) &&
        entt::locator<World>::has_value())
        World::Get().GetGameSettingsService().OnWindowPlacementChanged(uMsg);

    // Skyrim may toggle ShowCursor inside its own focus handling, so the
    // pointer owner re-applies its rule after the real WndProc has finished.
    InputService::AfterGameWndProc(hwnd, uMsg);

    return result;
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
    if (g_sRs)
        g_sRs->OnRender();

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
