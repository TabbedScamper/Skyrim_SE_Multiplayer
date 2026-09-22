#include <TiltedOnlinePCH.h>

#include <Games/Skyrim/Interface/IMenu.h>
#include <Games/Skyrim/Interface/MainMenuIntegration.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Games/Skyrim/BSGraphics/BSGraphicsRenderer.h>
#include <Games/TES.h>
#include <OverlayApp.hpp>
#include <Services/OverlayService.h>
#include <World.h>

namespace
{
struct ScaleformValue
{
    void* ObjectInterface{};
    uint32_t Type{3};
    uint32_t Pad{};
    union
    {
        double Number;
        void* Pointer;
    } Value{};
};
static_assert(sizeof(ScaleformValue) == 0x18);

using TGetVariable = bool(void*, ScaleformValue*, const char*);
using TSetVariable = bool(void*, const char*, const ScaleformValue&, uint32_t);

struct ScaleformViewport
{
    int32_t BufferWidth{};
    int32_t BufferHeight{};
    int32_t Left{};
    int32_t Top{};
    int32_t Width{};
    int32_t Height{};
    int32_t ScissorLeft{};
    int32_t ScissorTop{};
    int32_t ScissorWidth{};
    int32_t ScissorHeight{};
    float Scale{};
    float AspectRatio{};
    uint32_t Flags{};
    uint32_t Pad{};
};
static_assert(sizeof(ScaleformViewport) == 0x38);

using TSetViewport = void(void*, const ScaleformViewport&);
using TGetViewport = void(void*, ScaleformViewport*);
using TNotifyMouseState = void(void*, float, float, uint32_t, uint32_t);
using TGetViewScaleMode = uint32_t(void*);
using TGetViewAlignment = uint32_t(void*);
using TGetMouseState = void(void*, uint32_t, float*, float*, uint32_t*);
using TDisplay = void(void*);
using TSetViewScaleMode = void(void*, uint32_t);

struct GraphicsStateLayout
{
    uint8_t pad00[0x24]{};
    uint32_t ScreenWidth{};
    uint32_t ScreenHeight{};
    uint32_t FrameBufferWidth{};
    uint32_t FrameBufferHeight{};
};
static_assert(offsetof(GraphicsStateLayout, ScreenWidth) == 0x24);

struct MenuCursorLayout
{
    uint8_t pad00[4]{};
    float CursorX{};
    float CursorY{};
    float SafeZoneX{};
    float SafeZoneY{};
    float ScreenWidth{};
    float ScreenHeight{};
};
static_assert(offsetof(MenuCursorLayout, ScreenWidth) == 0x14);

void* s_pMainMenuMovie = nullptr;
bool s_mainMenuOverlayActive = false;
void* s_pVisibilityAppliedMovie = nullptr;
bool s_visibilityApplied = false;

void ApplyMainMenuVisibility()
{
    if (!s_pMainMenuMovie)
        return;

    // Main Menu.swf exposes its normal menu UI beneath MenuHolder. Never hide
    // the movie root: Scaleform can preserve root visibility through a render
    // target rebuild and leave the title screen permanently blank. The dragon
    // and smoke remain separate in Skyrim's native Mist Menu.
    if (s_pVisibilityAppliedMovie == s_pMainMenuMovie &&
        s_visibilityApplied == s_mainMenuOverlayActive)
        return;

    auto* pVtable = *reinterpret_cast<uintptr_t**>(s_pMainMenuMovie);
    if (!pVtable)
        return;

    const auto setVariable = reinterpret_cast<TSetVariable*>(pVtable[0x10]);
    ScaleformValue rootVisible{};
    rootVisible.Value.Number = 1.0;
    setVariable(s_pMainMenuMovie, "_root._visible", rootVisible, 0);

    ScaleformValue menuVisible{};
    menuVisible.Value.Number = s_mainMenuOverlayActive ? 0.0 : 1.0;
    if (setVariable(s_pMainMenuMovie, "_root.MenuHolder._visible", menuVisible, 0))
    {
        s_pVisibilityAppliedMovie = s_pMainMenuMovie;
        s_visibilityApplied = s_mainMenuOverlayActive;
    }
}

bool RefreshViewport(void* apMovie, uintptr_t* apVtable, int32_t aWidth, int32_t aHeight)
{
    ScaleformViewport viewport{};
    const auto getViewport = reinterpret_cast<TGetViewport*>(apVtable[0x1A]);
    getViewport(apMovie, &viewport);

    if (viewport.BufferWidth == aWidth && viewport.BufferHeight == aHeight &&
        viewport.Width == aWidth && viewport.Height == aHeight)
        return false;

    spdlog::info("Refreshing main-menu viewport {}x{} -> {}x{} (view {}x{})",
        viewport.BufferWidth, viewport.BufferHeight, aWidth, aHeight, viewport.Width, viewport.Height);
    viewport.BufferWidth = aWidth;
    viewport.BufferHeight = aHeight;
    viewport.Left = 0;
    viewport.Top = 0;
    viewport.Width = aWidth;
    viewport.Height = aHeight;
    viewport.ScissorLeft = 0;
    viewport.ScissorTop = 0;
    viewport.ScissorWidth = aWidth;
    viewport.ScissorHeight = aHeight;
    const auto setViewport = reinterpret_cast<TSetViewport*>(apVtable[0x19]);
    setViewport(apMovie, viewport);

    // SetViewport normally invalidates Scaleform's stage transform. Reapply
    // the movie's existing scale mode as well so a movie that survived a
    // swap-chain rebuild follows the same initialization path as LoadMovie.
    const auto getScaleMode = reinterpret_cast<TGetViewScaleMode*>(apVtable[0x1C]);
    const auto setScaleMode = reinterpret_cast<TSetViewScaleMode*>(apVtable[0x1B]);
    setScaleMode(apMovie, getScaleMode(apMovie));
    return true;
}

void OpenOptions()
{
    spdlog::info("Opening Skyrim SE Multiplayer options from the native main menu");
    auto& world = World::Get();
    auto& overlay = world.GetOverlayService();
    overlay.SetActive(true);
    if (auto* pApp = overlay.GetOverlayApp())
        pApp->ExecuteAsync("showTitleOptions");
    world.GetGameSettingsService().QueueRequestSettings();
}

void OpenCoopLobby()
{
    spdlog::info("Opening Skyrim SE Multiplayer co-op lobby from the native main menu");
    auto& world = World::Get();
    // Main-menu frames do not reliably emit the gameplay UpdateEvent used by
    // SteamLobbyService's background auto-host fallback. Starting here keeps
    // the UI's "Creating your private session" promise and is idempotent when
    // the player reopens the panel.
    world.GetSteamLobbyService().HostSession();
    auto& overlay = world.GetOverlayService();
    overlay.SetActive(true);
    if (auto* pApp = overlay.GetOverlayApp())
        pApp->ExecuteAsync("showTitleLobby");
    world.GetSteamLobbyService().RefreshLobbyState();
}

bool IsReadableMemory(const void* apMemory, size_t aSize)
{
    if (!apMemory || !aSize)
        return false;

    auto* cursor = static_cast<const uint8_t*>(apMemory);
    const auto* end = cursor + aSize;
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(cursor, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
        const auto* regionEnd = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
        cursor = std::min(regionEnd, end);
    }
    return true;
}

void DumpBytes(std::ofstream& aOutput, const char* acLabel, const void* apMemory, size_t aSize)
{
    aOutput << acLabel << " ptr=" << apMemory << " bytes=" << std::dec << aSize << '\n';
    if (!IsReadableMemory(apMemory, aSize))
    {
        aOutput << "  <memory is not readable>\n";
        return;
    }

    const auto* bytes = static_cast<const uint8_t*>(apMemory);
    for (size_t offset = 0; offset < aSize; offset += 16)
    {
        aOutput << fmt::format("  +0x{:03X}: ", offset);
        for (size_t index = 0; index < 16 && offset + index < aSize; ++index)
            aOutput << fmt::format("{:02X} ", bytes[offset + index]);
        aOutput << '\n';
    }
}

template <class T> T ReadAt(const void* apBase, size_t aOffset)
{
    T value{};
    const auto* address = static_cast<const uint8_t*>(apBase) + aOffset;
    if (IsReadableMemory(address, sizeof(T)))
        std::memcpy(&value, address, sizeof(T));
    return value;
}
}

void PollMainMenuOptions(IMenu* apMainMenu) noexcept
{
    if (!apMainMenu || !apMainMenu->uiMovie)
    {
        s_pMainMenuMovie = nullptr;
        s_pVisibilityAppliedMovie = nullptr;
        return;
    }

    auto* pMovie = apMainMenu->uiMovie;
    auto* pVtable = *reinterpret_cast<uintptr_t**>(pMovie);
    if (!pVtable)
        return;

    s_pMainMenuMovie = pMovie;
    ApplyMainMenuVisibility();

    // World::Update is gameplay-driven and may be dormant on the title
    // screen. Steam lobby create/join completion still requires
    // SteamAPI_RunCallbacks, so pump it from the main menu's live movie poll.
    World::Get().GetSteamLobbyService().PumpCallbacks();
    World::Get().GetTransport().PumpMainMenu();

    auto* pWindow = BSGraphics::GetMainWindow();
    if (pWindow && pWindow->pSwapChain)
    {
        DXGI_SWAP_CHAIN_DESC swapDesc{};
        if (SUCCEEDED(pWindow->pSwapChain->GetDesc(&swapDesc)) && swapDesc.BufferDesc.Width && swapDesc.BufferDesc.Height)
            RefreshViewport(pMovie, pVtable, static_cast<int32_t>(swapDesc.BufferDesc.Width),
                static_cast<int32_t>(swapDesc.BufferDesc.Height));
    }

    const auto getVariable = reinterpret_cast<TGetVariable*>(pVtable[0x11]);
    const auto setVariable = reinterpret_cast<TSetVariable*>(pVtable[0x10]);
    const auto consumeRequest = [&](const char* acVariable)
    {
        ScaleformValue request{};
        if (!getVariable(pMovie, &request, acVariable) || (request.Type & 0x0F) != 3 || request.Value.Number == 0.0)
            return false;
        ScaleformValue cleared{};
        cleared.Value.Number = 0.0;
        setVariable(pMovie, acVariable, cleared, 0);
        return true;
    };

    if (consumeRequest("_root.SkyrimSeamlessOptionsRequested"))
        OpenOptions();
    else if (consumeRequest("_root.SkyrimSeamlessCoopRequested"))
        OpenCoopLobby();
}

void SetMainMenuOverlayActive(bool aActive) noexcept
{
    s_mainMenuOverlayActive = aActive;
    ApplyMainMenuVisibility();
}

void LaunchSharedCampaignFromMainMenu(const uint8_t aCampaignMode) noexcept
{
    if (!s_pMainMenuMovie || (aCampaignMode != 1 && aCampaignMode != 2))
        return;
    auto* pVtable = *reinterpret_cast<uintptr_t**>(s_pMainMenuMovie);
    if (!pVtable)
        return;
    ScaleformValue mode{};
    mode.Value.Number = aCampaignMode;
    const auto setVariable = reinterpret_cast<TSetVariable*>(pVtable[0x10]);
    if (setVariable(s_pMainMenuMovie, "_root.SkyrimSeamlessLaunchMode", mode, 0))
        spdlog::info("Queued shared campaign launch from main menu (mode {})", aCampaignMode);
}

void SetMainMenuMouseState(float aX, float aY) noexcept
{
    // GFxMovieView::NotifyMouseState expects coordinates relative to the
    // current viewport. Skyrim draws its pointer through the separate global
    // Cursor Menu movie. Feed both movies the real position, while retaining a
    // zero button mask so clicks cannot leak through to hidden menu choices.
    void* movies[2]{s_pMainMenuMovie, nullptr};
    if (auto* pUI = UI::Get())
        if (auto* pCursorMenu = pUI->FindMenuByName(BSFixedString("Cursor Menu")))
            movies[1] = pCursorMenu->uiMovie;

    for (size_t index = 0; index < std::size(movies); ++index)
    {
        auto* pMovie = movies[index];
        if (!pMovie)
            continue;
        auto* pVtable = *reinterpret_cast<uintptr_t**>(pMovie);
        if (!pVtable)
            continue;
        const auto notifyMouseState = reinterpret_cast<TNotifyMouseState*>(pVtable[0x2F]);
        notifyMouseState(pMovie, aX, aY, 0, 0);
    }
}

void RenderNativeCursorOnTop() noexcept
{
    auto* pUI = UI::Get();
    if (!pUI)
        return;

    auto* pCursorMenu = pUI->FindMenuByName(BSFixedString("Cursor Menu"));
    auto* pMovie = pCursorMenu ? pCursorMenu->uiMovie : nullptr;
    if (!pMovie)
        return;

    auto* pVtable = *reinterpret_cast<uintptr_t**>(pMovie);
    if (!pVtable)
        return;

    // GFxMovieView::Display is vtable slot 0x26. OverlayService calls this
    // after CEF has rendered, making Skyrim's own cursor the final UI layer.
    reinterpret_cast<TDisplay*>(pVtable[0x26])(pMovie);
}

void RefreshMainMenu3DCamera() noexcept
{
    auto* pUI = UI::Get();
    auto* pMistMenu = pUI ? pUI->FindMenuByName(BSFixedString("Mist Menu")) : nullptr;
    if (!pMistMenu)
        return;

    // CommonLibSSE-NG's AE layout maps MistMenu::cameraFOV to +0x100; the
    // live 1.7.104 dump independently confirmed that offset and value.
    const float cameraFOV = *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(pMistMenu) + 0x100);
    if (!std::isfinite(cameraFOV) || cameraFOV < 1.f || cameraFOV > 179.f)
        return;

    POINTER_SKYRIMSE(void*, s_ui3DSceneManager, 403560);
    void* pManager = s_ui3DSceneManager.Get() ? *s_ui3DSceneManager.Get() : nullptr;
    if (!pManager)
        return;

    using TSetCameraFOV = void(void*, float);
    POINTER_SKYRIMSE(TSetCameraFOV, s_setCameraFOV, 52742);
    if (!s_setCameraFOV.Get())
        return;

    s_setCameraFOV.Get()(pManager, cameraFOV);

    spdlog::info("Refreshed main-menu UI3D camera projection at FOV {}", cameraFOV);
}

void RefreshMainMenuLayout() noexcept
{
    auto* pWindow = BSGraphics::GetMainWindow();
    auto* pUI = UI::Get();
    if (!pWindow || !pWindow->pSwapChain || !pUI)
        return;

    DXGI_SWAP_CHAIN_DESC swapDesc{};
    if (FAILED(pWindow->pSwapChain->GetDesc(&swapDesc)) || !swapDesc.BufferDesc.Width || !swapDesc.BufferDesc.Height)
        return;

    const auto width = static_cast<int32_t>(swapDesc.BufferDesc.Width);
    const auto height = static_cast<int32_t>(swapDesc.BufferDesc.Height);

    // BSScaleformManager::LoadMovie obtains both its viewport and aspect ratio
    // from BSGraphics::State. Keep that startup source of truth synchronized
    // before refreshing movies that remain alive across a runtime resize.
    static VersionDbPtr<uint8_t> s_graphicsState(411479);
    if (auto* pState = reinterpret_cast<GraphicsStateLayout*>(s_graphicsState.Get()))
    {
        pState->ScreenWidth = width;
        pState->ScreenHeight = height;
        pState->FrameBufferWidth = width;
        pState->FrameBufferHeight = height;
    }

    // MenuCursor is independent of Cursor Menu.swf. Its bounds are captured
    // from the display during title-screen initialization and otherwise keep
    // clamping the pointer to the former small top-left area.
    static VersionDbPtr<uint8_t> s_menuCursorSingleton(403551);
    auto** ppMenuCursor = reinterpret_cast<MenuCursorLayout**>(s_menuCursorSingleton.Get());
    if (auto* pMenuCursor = ppMenuCursor ? *ppMenuCursor : nullptr)
    {
        const float oldWidth = std::max(1.f, pMenuCursor->ScreenWidth);
        const float oldHeight = std::max(1.f, pMenuCursor->ScreenHeight);
        pMenuCursor->CursorX = std::clamp(pMenuCursor->CursorX * width / oldWidth, 0.f, static_cast<float>(width - 1));
        pMenuCursor->CursorY = std::clamp(pMenuCursor->CursorY * height / oldHeight, 0.f, static_cast<float>(height - 1));
        pMenuCursor->ScreenWidth = static_cast<float>(width);
        pMenuCursor->ScreenHeight = static_cast<float>(height);
        spdlog::info("Updated native MenuCursor bounds {}x{} -> {}x{}", oldWidth, oldHeight, width, height);
    }

    std::unordered_set<void*> refreshedMovies;
    uint32_t refreshedCount = 0;
    for (auto* pMenu : pUI->menuStack)
    {
        if (!pMenu || !pMenu->uiMovie || !refreshedMovies.emplace(pMenu->uiMovie).second)
            continue;

        auto* pVtable = *reinterpret_cast<uintptr_t**>(pMenu->uiMovie);
        if (pVtable && RefreshViewport(pMenu->uiMovie, pVtable, width, height))
        {
            ++refreshedCount;
            pMenu->RefreshPlatform();
        }
    }

    // Cursor Menu is a separate Scaleform movie. Leaving it at the old size
    // makes NotifyMouseState clamp to the former small top-left rectangle.
    // It normally lives in menuStack, but refresh it explicitly as a guard
    // against title-screen stack ordering differences.
    if (auto* pCursorMenu = pUI->FindMenuByName(BSFixedString("Cursor Menu"));
        pCursorMenu && pCursorMenu->uiMovie && refreshedMovies.emplace(pCursorMenu->uiMovie).second)
    {
        auto* pVtable = *reinterpret_cast<uintptr_t**>(pCursorMenu->uiMovie);
        if (pVtable && RefreshViewport(pCursorMenu->uiMovie, pVtable, width, height))
        {
            ++refreshedCount;
            pCursorMenu->RefreshPlatform();
        }
    }

    RefreshMainMenu3DCamera();

    spdlog::info("Reinitialized {} live Scaleform movies for {}x{}", refreshedCount, width, height);
}

void DumpMainMenuState(uint32_t aOverlayWidth, uint32_t aOverlayHeight) noexcept
{
    try
    {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        const auto directory = TiltedPhoques::GetPath() / "debug-feedback";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        const auto path = directory / fmt::format("main-menu-state-{:04}{:02}{:02}-{:02}{:02}{:02}-{}.txt",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId());
        std::ofstream output(path, std::ios::binary);
        if (!output)
        {
            spdlog::error("F9 main-menu dump could not open {}", path.string());
            return;
        }

        output << "Skyrim SE Multiplayer main-menu state dump\n";
        output << fmt::format("process={} overlay={}x{} foreground={}\n", GetCurrentProcessId(),
            aOverlayWidth, aOverlayHeight, static_cast<void*>(GetForegroundWindow()));

        auto* pWindow = BSGraphics::GetMainWindow();
        if (pWindow && pWindow->hWnd)
        {
            RECT windowRect{};
            RECT clientRect{};
            GetWindowRect(pWindow->hWnd, &windowRect);
            GetClientRect(pWindow->hWnd, &clientRect);
            output << fmt::format(
                "window hwnd={} renderer={}x{} rect=({},{}..{},{} {}x{}) client={}x{} style=0x{:X} exstyle=0x{:X} dpi={} foreground={}\n",
                static_cast<void*>(pWindow->hWnd), pWindow->uiWindowWidth, pWindow->uiWindowHeight,
                windowRect.left, windowRect.top, windowRect.right, windowRect.bottom,
                windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
                clientRect.right, clientRect.bottom,
                static_cast<uint64_t>(GetWindowLongPtrW(pWindow->hWnd, GWL_STYLE)),
                static_cast<uint64_t>(GetWindowLongPtrW(pWindow->hWnd, GWL_EXSTYLE)),
                GetDpiForWindow(pWindow->hWnd), GetForegroundWindow() == pWindow->hWnd);

            if (pWindow->pSwapChain)
            {
                DXGI_SWAP_CHAIN_DESC desc{};
                if (SUCCEEDED(pWindow->pSwapChain->GetDesc(&desc)))
                    output << fmt::format("swapchain buffer={}x{} format={} windowed={} swapEffect={} samples={} quality={}\n",
                        desc.BufferDesc.Width, desc.BufferDesc.Height, static_cast<uint32_t>(desc.BufferDesc.Format),
                        desc.Windowed, static_cast<uint32_t>(desc.SwapEffect), desc.SampleDesc.Count, desc.SampleDesc.Quality);
            }
        }

        auto* pRendererData = BSGraphics::GetRendererData();
        if (pRendererData)
        {
            output << fmt::format(
                "renderer fullscreen={} appFullscreen={} borderless={} resizeRequested={} requested={}x{} refresh={}/{} presentInterval={} context={}\n",
                pRendererData->bFullScreen, pRendererData->bAppFullScreen, pRendererData->bBorderlessWindow,
                pRendererData->bRequestWindowSizeChange, pRendererData->uiNewWidth, pRendererData->uiNewHeight,
                pRendererData->ActualRefreshRate, pRendererData->DesiredRefreshRate, pRendererData->uiPresentInterval,
                static_cast<void*>(pRendererData->pContext));

            if (auto* pContext = pRendererData->pContext)
            {
                UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
                D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
                pContext->RSGetViewports(&viewportCount, viewports);
                output << "d3d11 viewports=" << viewportCount << '\n';
                for (UINT index = 0; index < viewportCount; ++index)
                    output << fmt::format("  [{}] x={} y={} width={} height={} depth={}..{}\n", index,
                        viewports[index].TopLeftX, viewports[index].TopLeftY, viewports[index].Width,
                        viewports[index].Height, viewports[index].MinDepth, viewports[index].MaxDepth);

                UINT scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
                D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
                pContext->RSGetScissorRects(&scissorCount, scissors);
                output << "d3d11 scissors=" << scissorCount << '\n';
                for (UINT index = 0; index < scissorCount; ++index)
                    output << fmt::format("  [{}] {},{}..{},{} {}x{}\n", index, scissors[index].left,
                        scissors[index].top, scissors[index].right, scissors[index].bottom,
                        scissors[index].right - scissors[index].left, scissors[index].bottom - scissors[index].top);

                ID3D11RenderTargetView* pRenderTarget = nullptr;
                pContext->OMGetRenderTargets(1, &pRenderTarget, nullptr);
                if (pRenderTarget)
                {
                    ID3D11Resource* pResource = nullptr;
                    pRenderTarget->GetResource(&pResource);
                    if (pResource)
                    {
                        ID3D11Texture2D* pTexture = nullptr;
                        if (SUCCEEDED(pResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pTexture))))
                        {
                            D3D11_TEXTURE2D_DESC desc{};
                            pTexture->GetDesc(&desc);
                            output << fmt::format("d3d11 activeRT={}x{} format={} samples={} mips={} array={}\n",
                                desc.Width, desc.Height, static_cast<uint32_t>(desc.Format), desc.SampleDesc.Count,
                                desc.MipLevels, desc.ArraySize);
                            pTexture->Release();
                        }
                        pResource->Release();
                    }
                    pRenderTarget->Release();
                }
            }
        }

        auto* pUI = UI::Get();
        output << "\nUI singleton=" << pUI << '\n';
        IMenu* pMistMenu = nullptr;
        if (pUI)
        {
            output << fmt::format("UI stackSize={} visible={} modal={} counters pause={} item={} cursor={} custom={} app={}\n",
                pUI->menuStack.length, pUI->menuSystemVisible, pUI->modal, pUI->numPausesGame,
                pUI->numItemMenus, pUI->numDontHideCursorWhenTopmost, pUI->numCustomRendering,
                pUI->numApplicationMenus);
            for (const auto& entry : pUI->menuMap)
            {
                const char* pName = entry.key.AsAscii();
                auto* pMenu = entry.value.spMenu;
                output << fmt::format("menu name=\"{}\" instance={} creator={} open={} ",
                    pName ? pName : "<null>", static_cast<void*>(pMenu),
                    reinterpret_cast<void*>(entry.value.create), pMenu != nullptr);
                if (!pMenu)
                {
                    output << '\n';
                    continue;
                }
                output << fmt::format("movie={} depth={} flags=0x{:08X} inputContext={} refCount={}\n",
                    pMenu->uiMovie, static_cast<int32_t>(pMenu->depthPriority), pMenu->uiMenuFlags,
                    pMenu->eInputContext, pMenu->GetRefCount());
                if (pName && std::strcmp(pName, "Mist Menu") == 0)
                    pMistMenu = pMenu;

                auto* pMovie = pMenu->uiMovie;
                if (!pMovie || !IsReadableMemory(pMovie, sizeof(void*)))
                    continue;
                auto* pVtable = *reinterpret_cast<uintptr_t**>(pMovie);
                if (!IsReadableMemory(pVtable, sizeof(uintptr_t) * 0x30))
                {
                    output << "  movie vtable unreadable\n";
                    continue;
                }

                ScaleformViewport viewport{};
                reinterpret_cast<TGetViewport*>(pVtable[0x1A])(pMovie, &viewport);
                const auto scaleMode = reinterpret_cast<TGetViewScaleMode*>(pVtable[0x1C])(pMovie);
                const auto alignment = reinterpret_cast<TGetViewAlignment*>(pVtable[0x1E])(pMovie);
                float mouseX = 0.f;
                float mouseY = 0.f;
                uint32_t mouseButtons = 0;
                reinterpret_cast<TGetMouseState*>(pVtable[0x2E])(pMovie, 0, &mouseX, &mouseY, &mouseButtons);
                output << fmt::format(
                    "  movie vtable={} viewport buffer={}x{} view=({},{} {}x{}) scissor=({},{} {}x{}) scale={} aspect={} vpFlags=0x{:X} scaleMode={} alignment={} mouse=({},{}) buttons=0x{:X}\n",
                    static_cast<void*>(pVtable), viewport.BufferWidth, viewport.BufferHeight, viewport.Left,
                    viewport.Top, viewport.Width, viewport.Height, viewport.ScissorLeft, viewport.ScissorTop,
                    viewport.ScissorWidth, viewport.ScissorHeight, viewport.Scale, viewport.AspectRatio,
                    viewport.Flags, scaleMode, alignment, mouseX, mouseY, mouseButtons);
            }
        }

        output << "\nMain-menu 3D state\n";
        if (auto* pSettings = INISettingCollection::Get())
        {
            constexpr const char* cSettingNames[]{
                "fUIMistMenu_CameraFOV_G:Interface",
                "fUIMistMenu_CameraLookAtX_G:Interface",
                "fUIMistMenu_CameraLookAtY_G:Interface",
                "fUIMistMenu_CameraLookAtZ_G:Interface",
                "fUIMistMenu_CameraX_G:Interface",
                "fUIMistMenu_CameraY_G:Interface",
                "fUIMistMenu_CameraZ_G:Interface",
                "fUIMistMenu_DefaultLogoNIFScale:Interface",
                "fUIAltLogoModel_TranslateX_G:Interface",
                "fUIAltLogoModel_TranslateY_G:Interface",
                "fUIAltLogoModel_TranslateZ_G:Interface",
                "fUICameraNearDistance:Interface",
                "fUICameraFarDistance:Interface"
            };
            output << "engine settings\n";
            for (const auto* pName : cSettingNames)
            {
                const auto* pSetting = pSettings->GetSetting(pName);
                if (!pSetting)
                {
                    output << "  " << pName << "=<not found>\n";
                    continue;
                }
                const auto rawValue = static_cast<uint32_t>(pSetting->data);
                float floatValue = 0.f;
                std::memcpy(&floatValue, &rawValue, sizeof(floatValue));
                output << fmt::format("  {} float={} raw=0x{:016X}\n", pName, floatValue, pSetting->data);
            }
        }

        if (pMistMenu)
        {
            DumpBytes(output, "Mist Menu", pMistMenu, 0x150);
            output << fmt::format("Mist cameraFOV[+0x100]={} alternateCandidate[+0x110]={}\n",
                ReadAt<float>(pMistMenu, 0x100), ReadAt<float>(pMistMenu, 0x110));
        }
        else
            output << "Mist Menu is not instantiated\n";

        POINTER_SKYRIMSE(void*, s_ui3DSceneManager, 403560);
        void* pUI3DSceneManager = s_ui3DSceneManager.Get() ? *s_ui3DSceneManager.Get() : nullptr;
        output << "UI3DSceneManager singleton slot=" << static_cast<void*>(s_ui3DSceneManager.Get())
               << " instance=" << pUI3DSceneManager << '\n';
        if (pUI3DSceneManager)
        {
            DumpBytes(output, "UI3DSceneManager", pUI3DSceneManager, 0x118);
            output << fmt::format("UI3D camera[+0x20]={} cachedPosition[+0xC8]=({},{},{}) frustum[+0xF8]=({},{},{},{},{},{})\n",
                ReadAt<void*>(pUI3DSceneManager, 0x20), ReadAt<float>(pUI3DSceneManager, 0xC8),
                ReadAt<float>(pUI3DSceneManager, 0xCC), ReadAt<float>(pUI3DSceneManager, 0xD0),
                ReadAt<float>(pUI3DSceneManager, 0xF8), ReadAt<float>(pUI3DSceneManager, 0xFC),
                ReadAt<float>(pUI3DSceneManager, 0x100), ReadAt<float>(pUI3DSceneManager, 0x104),
                ReadAt<float>(pUI3DSceneManager, 0x108), ReadAt<float>(pUI3DSceneManager, 0x10C));
        }

        output.flush();
        spdlog::info("F9 main-menu state dump saved to {}", path.string());
    }
    catch (const std::exception& exception)
    {
        spdlog::error("F9 main-menu state dump failed: {}", exception.what());
    }
    catch (...)
    {
        spdlog::error("F9 main-menu state dump failed with an unknown exception");
    }
}
