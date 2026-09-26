#include <TiltedOnlinePCH.h>

#include <Services/InputService.h>
#include <Services/OverlayService.h>

#include <OverlayApp.hpp>

#include <DInputHook.hpp>
#include <WindowsHook.hpp>

#include <include/internal/cef_types.h>
#include <Services/ImguiService.h>
#include <Services/DiscordService.h>
#include <World.h>

#include "Games/Skyrim/Interface/MenuControls.h"
#include "Games/Skyrim/Interface/MainMenuIntegration.h"
#include <Games/Skyrim/Interface/UI.h>
#include <Games/Skyrim/Interface/IMenu.h>
#include <Games/Skyrim/BSGraphics/BSGraphicsRenderer.h>
#include <Games/Skyrim/Interface/ControlBindings.h>
#include <Games/Skyrim/Audio/AudioPreview.h>

#include <optional>

static OverlayService* s_pOverlay = nullptr;
static UINT s_currentACP = CP_ACP;
static LONG s_overlayMouseX = 0;
static LONG s_overlayMouseY = 0;
static bool s_overlayMouseInitialized = false;
static uint16_t s_overlayMouseWidth = 0;
static uint16_t s_overlayMouseHeight = 0;
static uint32_t s_overlayMouseButtonModifiers = 0;
static std::atomic_uint64_t s_lastControllerInputMs{0};

void InputService::NotifyControllerInput() noexcept
{
    s_lastControllerInputMs.store(GetTickCount64(), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Windows pointer ownership. One rule, evaluated on the window thread only:
//   game window focused (and not handed to the shell) -> Windows pointer hidden
//     and confined to the client area; Skyrim's Cursor Menu or CEF draws the
//     only visible cursor;
//   otherwise -> pointer free and visible.
// Vanilla Skyrim never calls ClipCursor (exe corpus: no callers), so without
// the confinement the real pointer leaves a windowed/borderless game and
// clicks land on other applications.
// Focus research (docs/REFERENCE_RESEARCH.md is outside this task's edit scope):
// SSEDisplayTweaks/SSETweaks/window.cpp:93-131,220-228,406-414 releases on
// deactivation and runs after vanilla. Adopt those two rules, but gate resize
// recapture on focus as well. Source:
// https://github.com/SlavicPotato/SSEDisplayTweaks/blob/master/SSETweaks/window.cpp
// Native WndProc VA 14065f270 (ID 36649) shows on WA_INACTIVE but hides on
// client WM_NCHITTEST. MenuCursor VA 14117a5e0 (ID 82541) caches ShowCursor.
// Mouse init VA 140e13080 (ID 68801) uses foreground cooperative level 5/6;
// keyboard init VA 140e126f0 (ID 68781) uses 0x15 before our existing patch.
// Foreground DirectInput automatically unacquires on deactivation. Reject new
// cooperative-level patches: suspend our DInputHook raw registration instead,
// then let native GetDeviceState VA 140cfaee0 (ID 68677) reacquire on return.
// ---------------------------------------------------------------------------
namespace
{
struct MenuCursorCounter
{
    uint8_t Pad[0x2C];
    int32_t ShowCursorCount; // CommonLibSSE-NG MenuCursor::showCursorCount
};
static_assert(offsetof(MenuCursorCounter, ShowCursorCount) == 0x2C);

std::atomic_bool s_shellOwnsPointer{false}; // Also read by the game input poll.
std::atomic_bool s_windowDeactivated{false};
uint32_t s_focusMessageDepth = 0;
std::optional<bool> s_appliedGameOwnership; // last visibility state applied
std::optional<int> s_savedCursorCount;
HCURSOR s_savedCursor = nullptr;
std::optional<bool> s_savedOverlayCursor;
bool s_savedOverlayActive = false;
std::optional<bool> s_overlayFocused;
uint64_t s_nextClipCheckMs = 0;

// ShowCursor has no count getter. This balanced pair leaves it unchanged.
int GetSystemCursorCount() noexcept
{
    ShowCursor(FALSE);
    return ShowCursor(TRUE);
}

void SetSystemCursorCount(int aTarget) noexcept
{
    int count = GetSystemCursorCount();
    while (count < aTarget)
        count = ShowCursor(TRUE);
    while (count > aTarget)
        count = ShowCursor(FALSE);

    // MenuCursor::ShowSystemCursor (VA 14117a5e0, ID 82541) uses this cache.
    static VersionDbPtr<uint8_t> s_menuCursorSingleton(403551);
    auto** ppMenuCursor = reinterpret_cast<MenuCursorCounter**>(s_menuCursorSingleton.Get());
    if (ppMenuCursor && *ppMenuCursor)
        (*ppMenuCursor)->ShowCursorCount = count;
}

void SetSystemCursorVisible(bool aVisible) noexcept
{
    const int count = GetSystemCursorCount();
    SetSystemCursorCount(aVisible ? std::max(0, count) : std::min(-1, count));
}

void ClipToClientArea(HWND aWindow) noexcept
{
    RECT client{};
    if (!GetClientRect(aWindow, &client) || client.right <= client.left || client.bottom <= client.top)
        return;
    POINT topLeft{client.left, client.top};
    POINT bottomRight{client.right, client.bottom};
    ClientToScreen(aWindow, &topLeft);
    ClientToScreen(aWindow, &bottomRight);
    const RECT screen{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    RECT current{};
    if (GetClipCursor(&current) && EqualRect(&current, &screen))
        return;
    ClipCursor(&screen);
}

bool GameOwnsPointer(HWND aWindow) noexcept
{
    return !s_shellOwnsPointer && !s_windowDeactivated && s_focusMessageDepth == 0 &&
        GetForegroundWindow() == aWindow && GetFocus() == aWindow && !IsIconic(aWindow);
}

void UpdateOverlayFocus(bool aOwned) noexcept
{
    const bool active = s_pOverlay && s_pOverlay->GetActive();
    const bool focused = aOwned && active;
    auto& input = TiltedPhoques::DInputHook::Get();
    if (input.IsEnabled() != focused)
        input.SetEnabled(focused);

    const auto app = s_pOverlay ? s_pOverlay->GetOverlayApp() : nullptr;
    const auto client = app ? app->GetClient() : nullptr;
    const auto browser = client ? client->GetBrowser() : nullptr;
    if (browser && s_overlayFocused != focused)
    {
        browser->GetHost()->SetFocus(focused);
        if (!focused)
            browser->GetHost()->SendCaptureLostEvent();
        s_overlayFocused = focused;
    }
}

void UpdateCursorOwnership(HWND aWindow) noexcept
{
    const bool owned = GameOwnsPointer(aWindow);
    const bool changed = s_appliedGameOwnership != owned;
    if (!owned && changed)
    {
        // Save before vanilla handles WM_ACTIVATE, which changes ShowCursor.
        if (s_appliedGameOwnership.value_or(false))
        {
            s_savedCursorCount = GetSystemCursorCount();
            s_savedCursor = GetCursor();
        }
        s_savedOverlayActive = s_pOverlay && s_pOverlay->GetActive();
        const auto app = s_pOverlay ? s_pOverlay->GetOverlayApp() : nullptr;
        if (const auto client = app ? app->GetClient() : nullptr)
        {
            if (const auto renderer = client->GetOverlayRenderHandler())
            {
                s_savedOverlayCursor = renderer->IsCursorVisible();
                renderer->SetCursorVisible(false);
            }
        }
        s_overlayMouseButtonModifiers = 0;
        s_overlayMouseInitialized = false;
        // SetEnabled(false) removes our raw registration and unacquires even
        // when the overlay was already closed. Native polling reacquires later.
        TiltedPhoques::DInputHook::Get().SetEnabled(false);
        if (GetCapture() == aWindow)
            ReleaseCapture();
    }
    s_appliedGameOwnership = owned;
    UpdateOverlayFocus(owned);
    if (owned)
    {
        ClipToClientArea(aWindow);
        if (changed)
        {
            if (s_savedCursorCount)
            {
                SetSystemCursorCount(*s_savedCursorCount);
                SetCursor(s_savedCursor);
                s_savedCursorCount.reset();
            }
            else
                SetSystemCursorVisible(false);
            const auto app = s_pOverlay ? s_pOverlay->GetOverlayApp() : nullptr;
            if (const auto client = app ? app->GetClient() : nullptr)
                if (const auto renderer = client->GetOverlayRenderHandler(); renderer && s_savedOverlayCursor)
                    renderer->SetCursorVisible(s_savedOverlayActive == s_pOverlay->GetActive() && *s_savedOverlayCursor);
            s_savedOverlayCursor.reset();
        }
    }
    else
    {
        // Reassert after vanilla's focus/hit-test handling, without recapturing
        // on a stale foreground HWND or a queued resize/raw-input message.
        ClipCursor(nullptr);
        SetSystemCursorVisible(true);
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
    }
    if (changed)
        spdlog::info("Windows pointer {}", owned ? "confined to game" : "released to desktop");
}

void HandPointerToShell(HWND aWindow) noexcept
{
    s_shellOwnsPointer = true;
    UpdateCursorOwnership(aWindow);
}

// Exactly one visible cursor: Skyrim's Cursor Menu when it is open (it is
// drawn over CEF by OverlayService), otherwise CEF's own software cursor. In
// gameplay no Skyrim menu is open, so hiding CEF's cursor left none at all.
bool IsNativeCursorMenuOpen() noexcept
{
    auto* pUI = UI::Get();
    auto* pCursorMenu = pUI ? pUI->FindMenuByName(BSFixedString("Cursor Menu")) : nullptr;
    return pCursorMenu && pCursorMenu->uiMovie;
}
} // namespace

namespace
{
// bBackgroundMouse:Controls (Setting ID 388492, bool value at +8). Skyrim
// reads it when it opens the mouse (FUN_140e13080: DirectInput cooperative
// level 6 = non-exclusive instead of 5 = exclusive) and in its menu cursor
// update (FUN_14117a340: cursor = GetCursorPos in the client area instead of
// raw deltas x fMouseCursorSpeed). With it on, every cursor follows the real
// Windows pointer: Windows speed/acceleration apply and remote-desktop input
// (absolute positions) no longer moves the cursor in large steps. The camera
// still reads raw DirectInput deltas. Confinement comes from UpdateCursorOwnership.
bool* BackgroundMouseFlag() noexcept
{
    static VersionDbPtr<uint8_t> s_setting(388492);
    auto* pSetting = s_setting.Get();
    return pSetting ? reinterpret_cast<bool*>(pSetting + 8) : nullptr;
}

bool UsesSystemPointer() noexcept
{
    const auto* pFlag = BackgroundMouseFlag();
    return pFlag && *pFlag;
}
} // namespace

// Before the game creates its mouse device. No shipped INI sets the key, so
// the value survives INI loading unless a player sets bBackgroundMouse=0.
static TiltedPhoques::Initializer s_backgroundMouse([]() {
    if (auto* pFlag = BackgroundMouseFlag())
        *pFlag = true;
});

bool InputService::IsPointerHandedToShell() noexcept
{
    return s_shellOwnsPointer || s_windowDeactivated;
}

void InputService::RequestCursorUpdate() noexcept
{
    if (const auto* pWindow = BSGraphics::GetMainWindow(); pWindow && pWindow->hWnd)
        PostMessageW(pWindow->hWnd, cCursorUpdateMessage, 0, 0);
}

void InputService::AfterGameWndProc(HWND hwnd, UINT uMsg) noexcept
{
    switch (uMsg)
    {
    case WM_ACTIVATE:
    case WM_ACTIVATEAPP:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (s_focusMessageDepth != 0)
            --s_focusMessageDepth;
        if (s_focusMessageDepth == 0)
            UpdateCursorOwnership(hwnd);
        break;
    case WM_SIZE:
    case WM_MOVE:
    case WM_WINDOWPOSCHANGED:
    case WM_DISPLAYCHANGE:
        // Run after native focus handling, preserving the pre-handoff count.
        UpdateCursorOwnership(hwnd);
        break;
    default: break;
    }
}

POINT MapClientToOverlay(HWND aWindow, POINT aPosition, TiltedPhoques::OverlayRenderHandler* apRenderer)
{
    RECT client{};
    if (!apRenderer || !GetClientRect(aWindow, &client))
        return aPosition;

    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;
    const auto [overlayWidth, overlayHeight] = apRenderer->GetRenderSize();
    if (clientWidth <= 0 || clientHeight <= 0 || overlayWidth == 0 || overlayHeight == 0)
        return aPosition;

    aPosition.x = std::clamp<LONG>(aPosition.x, 0, static_cast<LONG>(clientWidth - 1));
    aPosition.y = std::clamp<LONG>(aPosition.y, 0, static_cast<LONG>(clientHeight - 1));
    aPosition.x = static_cast<LONG>((static_cast<int64_t>(aPosition.x) * overlayWidth) / clientWidth);
    aPosition.y = static_cast<LONG>((static_cast<int64_t>(aPosition.y) * overlayHeight) / clientHeight);

    static int s_lastClientWidth = 0;
    static int s_lastClientHeight = 0;
    static uint32_t s_lastOverlayWidth = 0;
    static uint32_t s_lastOverlayHeight = 0;
    if (clientWidth != s_lastClientWidth || clientHeight != s_lastClientHeight ||
        overlayWidth != s_lastOverlayWidth || overlayHeight != s_lastOverlayHeight)
    {
        spdlog::info("Mouse coordinate mapping: client {}x{} -> overlay {}x{}",
            clientWidth, clientHeight, overlayWidth, overlayHeight);
        s_lastClientWidth = clientWidth;
        s_lastClientHeight = clientHeight;
        s_lastOverlayWidth = overlayWidth;
        s_lastOverlayHeight = overlayHeight;
    }

    return aPosition;
}

POINT GetOverlayMousePosition(TiltedPhoques::OverlayRenderHandler* apRenderer)
{
    const auto [width, height] = apRenderer->GetRenderSize();
    if (!s_overlayMouseInitialized)
    {
        s_overlayMouseX = static_cast<LONG>(width / 2);
        s_overlayMouseY = static_cast<LONG>(height / 2);
        s_overlayMouseWidth = width;
        s_overlayMouseHeight = height;
        s_overlayMouseInitialized = true;
        spdlog::info("Overlay raw cursor initialized at {},{} in {}x{}", s_overlayMouseX, s_overlayMouseY, width, height);
    }
    else if (width && height && (width != s_overlayMouseWidth || height != s_overlayMouseHeight))
    {
        const auto oldWidth = std::max<uint16_t>(1, s_overlayMouseWidth);
        const auto oldHeight = std::max<uint16_t>(1, s_overlayMouseHeight);
        s_overlayMouseX = static_cast<LONG>((static_cast<int64_t>(s_overlayMouseX) * width) / oldWidth);
        s_overlayMouseY = static_cast<LONG>((static_cast<int64_t>(s_overlayMouseY) * height) / oldHeight);
        s_overlayMouseWidth = width;
        s_overlayMouseHeight = height;
        spdlog::info("Overlay cursor rescaled to {},{} in {}x{}", s_overlayMouseX, s_overlayMouseY, width, height);
    }
    return {s_overlayMouseX, s_overlayMouseY};
}

POINT AdvanceOverlayMouse(HWND aWindow, const RAWMOUSE& acMouse, TiltedPhoques::OverlayRenderHandler* apRenderer)
{
    POINT position = GetOverlayMousePosition(apRenderer);
    const auto [overlayWidth, overlayHeight] = apRenderer->GetRenderSize();
    RECT client{};
    GetClientRect(aWindow, &client);
    const int clientWidth = std::max(1L, client.right - client.left);
    const int clientHeight = std::max(1L, client.bottom - client.top);

    if ((acMouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
    {
        const auto deltaX = static_cast<LONG>((static_cast<int64_t>(acMouse.lLastX) * overlayWidth) / clientWidth);
        const auto deltaY = static_cast<LONG>((static_cast<int64_t>(acMouse.lLastY) * overlayHeight) / clientHeight);
        s_overlayMouseX = std::clamp<LONG>(s_overlayMouseX + deltaX, 0, static_cast<LONG>(std::max(1u, overlayWidth) - 1));
        s_overlayMouseY = std::clamp<LONG>(s_overlayMouseY + deltaY, 0, static_cast<LONG>(std::max(1u, overlayHeight) - 1));
    }
    else
    {
        // Remote Desktop, accessibility software, and some high-resolution
        // devices report absolute raw coordinates. Treating those values as
        // deltas leaves the menu cursor permanently at its initial center.
        const bool virtualDesktop = (acMouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        const int originX = virtualDesktop ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0;
        const int originY = virtualDesktop ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;
        const int screenWidth = std::max(1, GetSystemMetrics(virtualDesktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN));
        const int screenHeight = std::max(1, GetSystemMetrics(virtualDesktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN));
        POINT screenPosition{
            originX + static_cast<LONG>((static_cast<int64_t>(acMouse.lLastX) * (screenWidth - 1)) / 65535),
            originY + static_cast<LONG>((static_cast<int64_t>(acMouse.lLastY) * (screenHeight - 1)) / 65535)};
        ScreenToClient(aWindow, &screenPosition);
        position = MapClientToOverlay(aWindow, screenPosition, apRenderer);
        s_overlayMouseX = position.x;
        s_overlayMouseY = position.y;
    }

    return {s_overlayMouseX, s_overlayMouseY};
}

void ForceKillAllInput()
{
    MenuControls::GetInstance()->SetToggle(false);
}

uint32_t GetCefModifiers(uint16_t aVirtualKey)
{
    uint32_t modifiers = s_overlayMouseButtonModifiers;

    if (GetKeyState(VK_MENU) & 0x8000)
    {
        modifiers |= EVENTFLAG_ALT_DOWN;
    }

    if (GetKeyState(VK_CONTROL) & 0x8000)
    {
        modifiers |= EVENTFLAG_CONTROL_DOWN;
    }

    if (GetKeyState(VK_SHIFT) & 0x8000)
    {
        modifiers |= EVENTFLAG_SHIFT_DOWN;
    }

    if (GetKeyState(VK_CAPITAL) & 1)
    {
        modifiers |= EVENTFLAG_CAPS_LOCK_ON;
    }

    if (GetKeyState(VK_NUMLOCK) & 1)
    {
        modifiers |= EVENTFLAG_NUM_LOCK_ON;
    }

    if (aVirtualKey)
    {
        if (aVirtualKey == VK_RCONTROL || aVirtualKey == VK_RMENU || aVirtualKey == VK_RSHIFT)
        {
            modifiers |= EVENTFLAG_IS_RIGHT;
        }
        else if (aVirtualKey == VK_LCONTROL || aVirtualKey == VK_LMENU || aVirtualKey == VK_LSHIFT)
        {
            modifiers |= EVENTFLAG_IS_LEFT;
        }
        else if (aVirtualKey >= VK_NUMPAD0 && aVirtualKey <= VK_DIVIDE)
        {
            modifiers |= EVENTFLAG_IS_KEY_PAD;
        }
    }

    return modifiers;
}

// remember to update this when updating toggle keys
bool IsToggleKey(int aKey) noexcept
{
    return aKey == VK_RCONTROL || aKey == VK_F2;
}

bool IsDisableKey(int aKey) noexcept
{
    return aKey == VK_ESCAPE;
}

void SetUIActive(OverlayService& aOverlay, auto apRenderer, bool aActive)
{
    aOverlay.SetActive(aActive);

    // Ensures the UI receives the current shell state if the initial event was sent too early.
    aOverlay.SetVersion(BUILD_COMMIT);
    aOverlay.GetOverlayApp()->ExecuteAsync(aOverlay.GetInGame() ? "enterGame" : "enterTitleScreen");

    apRenderer->SetCursorVisible(aActive && !IsNativeCursorMenuOpen());
    InputService::RequestCursorUpdate();
}

void ProcessKeyboard(uint16_t aKey, uint16_t aScanCode, cef_key_event_type_t aType, bool aE0, bool aE1)
{
    if (aType != KEYEVENT_CHAR)
    {
        if (!aKey || aKey == 255)
        {
            return;
        }

        if (aKey == VK_SHIFT)
        {
            aKey = static_cast<uint16_t>(MapVirtualKey(aScanCode, MAPVK_VSC_TO_VK_EX));
        }
        else if (aKey == VK_NUMLOCK)
        {
            aScanCode = static_cast<uint16_t>(MapVirtualKey(aKey, MAPVK_VK_TO_VSC) | 0x100);
        }

        if (aE1)
        {
            if (aKey == VK_PAUSE)
            {
                aScanCode = 0x45;
            }
            else
            {
                aScanCode = static_cast<uint16_t>(MapVirtualKey(aKey, MAPVK_VK_TO_VSC));
            }
        }

        if (aE0)
        {
            switch (aKey)
            {
            case VK_CONTROL: aKey = VK_RCONTROL; break;
            case VK_MENU: aKey = VK_RMENU; break;
            case VK_RETURN: aKey = VK_SEPARATOR; break;
            }
        }
        else
        {
            switch (aKey)
            {
            case VK_CONTROL: aKey = VK_LCONTROL; break;
            case VK_MENU: aKey = VK_LMENU; break;
            case VK_INSERT: aKey = VK_NUMPAD0; break;
            case VK_DELETE: aKey = VK_DECIMAL; break;
            case VK_HOME: aKey = VK_NUMPAD7; break;
            case VK_END: aKey = VK_NUMPAD1; break;
            case VK_PRIOR: aKey = VK_NUMPAD9; break;
            case VK_NEXT: aKey = VK_NUMPAD3; break;
            case VK_LEFT: aKey = VK_NUMPAD4; break;
            case VK_RIGHT: aKey = VK_NUMPAD6; break;
            case VK_UP: aKey = VK_NUMPAD8; break;
            case VK_DOWN: aKey = VK_NUMPAD2; break;
            case VK_CLEAR: aKey = VK_NUMPAD5; break;
            }
        }
    }

    auto& overlay = *s_pOverlay;

    const auto pApp = overlay.GetOverlayApp();
    if (!pApp)
        return;

    const auto pClient = pApp->GetClient();
    if (!pClient)
        return;

    const auto pRenderer = pClient->GetOverlayRenderHandler();
    if (!pRenderer)
        return;

    const auto active = overlay.GetActive();

    spdlog::debug("ProcessKey, type: {}, key: {}, active: {}", aType, aKey, active);

    if (aType != KEYEVENT_CHAR && (IsToggleKey(aKey) || (IsDisableKey(aKey) && active)))
    {
        if (!overlay.GetInGame() && !overlay.GetTitleScreen())
        {
            TiltedPhoques::DInputHook::Get().SetEnabled(false);
        }
        else if (aType == KEYEVENT_KEYUP)
        {
            SetUIActive(overlay, pRenderer, !active);
        }
    }
    else if (active)
    {
        pApp->InjectKey(aType, GetCefModifiers(aKey), aKey, aScanCode);
    }
}

void ProcessMouseMove(uint16_t aX, uint16_t aY)
{
    auto& overlay = *s_pOverlay;

    const auto pApp = overlay.GetOverlayApp();
    if (!pApp)
        return;

    const auto pClient = pApp->GetClient();
    if (!pClient)
        return;

    const auto pRenderer = pClient->GetOverlayRenderHandler();
    if (!pRenderer)
        return;

    const auto active = overlay.GetActive();

    if (active || overlay.GetTitleScreen())
    {
        pApp->InjectMouseMove(aX, aY, GetCefModifiers(0));
    }
}

void ProcessMouseButton(uint16_t aX, uint16_t aY, cef_mouse_button_type_t aButton, bool aDown)
{
    auto& overlay = *s_pOverlay;

    const auto pApp = overlay.GetOverlayApp();
    if (!pApp)
        return;

    const auto pClient = pApp->GetClient();
    if (!pClient)
        return;

    const auto pRenderer = pClient->GetOverlayRenderHandler();
    if (!pRenderer)
        return;

    const auto active = overlay.GetActive();

    if (active || overlay.GetTitleScreen())
    {
        pApp->InjectMouseButton(aX, aY, aButton, !aDown, GetCefModifiers(0));
    }
}

void ProcessMouseWheel(uint16_t aX, uint16_t aY, int16_t aZ)
{
    auto& overlay = *s_pOverlay;

    const auto pApp = overlay.GetOverlayApp();
    if (!pApp)
        return;

    const auto pClient = pApp->GetClient();
    if (!pClient)
        return;

    const auto pRenderer = pClient->GetOverlayRenderHandler();
    if (!pRenderer)
        return;

    const auto active = overlay.GetActive();

    if (active)
    {
        pApp->InjectMouseWheel(aX, aY, aZ, GetCefModifiers(0));
    }
}

UINT GetRealACP()
{
    // Get the keyboard layout for the current thread.
    HKL keybdLayout = GetKeyboardLayout(0);

    // Extract the language ID from it, contained in its low-order word.
    int langID = LOWORD(keybdLayout);

    // Call the GetLocaleInfo function to retrieve the default ANSI code page
    // associated with that language ID.
    UINT acp = CP_ACP;
    GetLocaleInfo(MAKELCID(langID, SORT_DEFAULT),
        LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
        (LPTSTR) &acp,
        sizeof(acp) / sizeof(TCHAR));
    return acp;
}

LRESULT CALLBACK InputService::WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_TIMER && wParam == AudioPreview::kTimerId)
    {
        AudioPreview::Tick();
        return 1;
    }

    if (uMsg == cCursorUpdateMessage)
    {
        UpdateCursorOwnership(hwnd);
        return 1;
    }

    // Native WM_ACTIVATE can send nested WM_SETFOCUS. Restore only after the
    // outer handler has finished changing the cursor and its cached count.
    if (uMsg == WM_ACTIVATE || uMsg == WM_ACTIVATEAPP || uMsg == WM_SETFOCUS || uMsg == WM_KILLFOCUS)
        ++s_focusMessageDepth;

    // Deactivation is authoritative even before GetForegroundWindow changes.
    if ((uMsg == WM_ACTIVATE && (LOWORD(wParam) == WA_INACTIVE || HIWORD(wParam))) ||
        (uMsg == WM_ACTIVATEAPP && wParam == FALSE) || uMsg == WM_KILLFOCUS ||
        (uMsg == WM_SIZE && wParam == SIZE_MINIMIZED) || uMsg == WM_DESTROY)
    {
        s_windowDeactivated = true;
        UpdateCursorOwnership(hwnd);
    }
    else if ((uMsg == WM_ACTIVATE && LOWORD(wParam) != WA_INACTIVE) ||
        uMsg == WM_SETFOCUS || (uMsg == WM_ACTIVATEAPP && wParam != FALSE))
    {
        s_windowDeactivated = false;
        if (GetForegroundWindow() == hwnd &&
            !(GetAsyncKeyState(VK_LWIN) & 0x8000) && !(GetAsyncKeyState(VK_RWIN) & 0x8000))
            s_shellOwnsPointer = false;
    }
    else if (uMsg == WM_LBUTTONDOWN && GetForegroundWindow() == hwnd && GetFocus() == hwnd)
    {
        s_shellOwnsPointer = false;
        s_windowDeactivated = false;
        UpdateCursorOwnership(hwnd);
    }

    // Windows-key shell UI (Start, Win+Z Snap layouts) needs a free pointer
    // even though Skyrim keeps focus.
    if ((uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) && (wParam == VK_LWIN || wParam == VK_RWIN))
        HandPointerToShell(hwnd);

    RAWINPUT input{};
    if (uMsg == WM_INPUT)
    {
        UINT size = sizeof(input);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) == UINT(-1))
            return 0;
        // Check before binding capture or clipping maintenance. The escape key
        // must never become a binding or get forwarded to the browser.
        if (input.header.dwType == RIM_TYPEKEYBOARD && (input.data.keyboard.Flags & RI_KEY_BREAK) == 0 &&
            (input.data.keyboard.VKey == VK_LWIN || input.data.keyboard.VKey == VK_RWIN))
            HandPointerToShell(hwnd);
    }

    // Skyrim's own WndProc (FUN_14065f270) answers WM_NCHITTEST and hides the
    // pointer whenever it is over the client area, which is sent on every mouse
    // move. While the desktop owns the pointer, answer it ourselves so the
    // Windows cursor stays visible over the game.
    if (uMsg == WM_NCHITTEST && !GameOwnsPointer(hwnd))
    {
        const LRESULT hit = DefWindowProcW(hwnd, uMsg, wParam, lParam);
        if (hit != HTNOWHERE)
        {
            UpdateCursorOwnership(hwnd);
            return hit;
        }
    }

    if (uMsg == WM_SETCURSOR)
    {
        // Both foreground ownership and the deactivation latch must agree.
        UpdateCursorOwnership(hwnd);
        if (LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(GameOwnsPointer(hwnd) ? nullptr : LoadCursor(nullptr, IDC_ARROW));
            return TRUE;
        }
    }

    // Windows drops ClipCursor on some shell events without messaging us;
    // re-check cheaply while input is flowing.
    if (uMsg == WM_INPUT)
    {
        const auto now = GetTickCount64();
        if (now >= s_nextClipCheckMs)
        {
            s_nextClipCheckMs = now + 250;
            UpdateCursorOwnership(hwnd);
        }
    }

    // Let the native WndProc receive focus and raw-input cleanup messages, but
    // never inject shell input into CEF, ImGui, or an armed binding capture.
    if (!GameOwnsPointer(hwnd) || !s_pOverlay)
        return 0;

    const auto pApp = s_pOverlay->GetOverlayApp();
    if (!pApp)
        return 0;

    const auto pClient = pApp->GetClient();
    if (!pClient)
        return 0;

    const auto pRenderer = pClient->GetOverlayRenderHandler();
    if (!pRenderer)
        return 0;

    auto& discord = World::Get().ctx().at<DiscordService>();
    discord.WndProcHandler(hwnd, uMsg, wParam, lParam);

    const bool active = s_pOverlay->GetActive();
    if (!active)
    {
        s_overlayMouseInitialized = false;
        s_overlayMouseButtonModifiers = 0;
    }
    if (active)
    {
        auto& imgui = World::Get().ctx().at<ImguiService>();
        imgui.WndProcHandler(hwnd, uMsg, wParam, lParam);
    }

    POINT position;

    GetCursorPos(&position);
    ScreenToClient(hwnd, &position);
    position = MapClientToOverlay(hwnd, position, pRenderer.get());

    const bool systemPointer = UsesSystemPointer();
    if (active)
    {
        if (systemPointer)
        {
            // Keep the raw accumulator in step so a fallback never jumps.
            s_overlayMouseX = position.x;
            s_overlayMouseY = position.y;
        }
        else
            position = GetOverlayMousePosition(pRenderer.get());
        if (s_pOverlay->GetTitleScreen())
            SetMainMenuMouseState(static_cast<float>(position.x), static_cast<float>(position.y));
    }

    ProcessMouseMove(static_cast<uint16_t>(position.x), static_cast<uint16_t>(position.y));

    if (uMsg == WM_INPUT)
    {
        if (active)
        {
            auto& imgui = World::Get().ctx().at<ImguiService>();
            imgui.RawInputHandler(input);
        }

        // An armed key-binding capture consumes the next key/button press.
        if (ControlBindings::IsCapturing())
        {
            if (input.header.dwType == RIM_TYPEKEYBOARD && (input.data.keyboard.Flags & RI_KEY_BREAK) == 0 &&
                ControlBindings::OnKeyboardScanCode(input.data.keyboard.MakeCode, (input.data.keyboard.Flags & RI_KEY_E0) != 0))
                return 1;
            if (input.header.dwType == RIM_TYPEMOUSE)
            {
                const auto flags = input.data.mouse.usButtonFlags;
                const int button = (flags & RI_MOUSE_LEFT_BUTTON_DOWN) ? 0 : (flags & RI_MOUSE_RIGHT_BUTTON_DOWN) ? 1 :
                    (flags & RI_MOUSE_MIDDLE_BUTTON_DOWN) ? 2 : (flags & RI_MOUSE_BUTTON_4_DOWN) ? 3 :
                    (flags & RI_MOUSE_BUTTON_5_DOWN) ? 4 : -1;
                if (button >= 0 && ControlBindings::OnMouseButton(static_cast<uint32_t>(button)))
                    return 1;
            }
        }

        if (input.header.dwType == RIM_TYPEKEYBOARD)
        {
            const auto keyboard = input.data.keyboard;

            ProcessKeyboard(keyboard.VKey, keyboard.MakeCode, keyboard.Flags & RI_KEY_BREAK ? KEYEVENT_KEYUP : KEYEVENT_KEYDOWN, keyboard.Flags & RI_KEY_E0, keyboard.Flags & RI_KEY_E1);
        }
        else if (input.header.dwType == RIM_TYPEMOUSE)
        {
            const auto mouse = input.data.mouse;

            if (active && !s_shellOwnsPointer)
            {
                // One visible cursor: CEF draws its own only when Skyrim's
                // Cursor Menu is not open to draw the native one.
                pRenderer->SetCursorVisible(!IsNativeCursorMenuOpen());
                if (!systemPointer)
                    position = AdvanceOverlayMouse(hwnd, mouse, pRenderer.get());
                if (s_pOverlay->GetTitleScreen())
                    SetMainMenuMouseState(static_cast<float>(position.x), static_cast<float>(position.y));
                ProcessMouseMove(static_cast<uint16_t>(position.x), static_cast<uint16_t>(position.y));
            }

            if (mouse.usButtonFlags & RI_MOUSE_WHEEL)
            {
                ProcessMouseWheel(static_cast<uint16_t>(position.x), static_cast<uint16_t>(position.y), (int16_t)mouse.usButtonData);
            }

            if (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)
            {
                s_overlayMouseButtonModifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
                ProcessMouseButton(static_cast<uint16_t>(position.x), static_cast<uint16_t>(position.y), MBT_LEFT, true);
            }

            if (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)
            {
                s_overlayMouseButtonModifiers &= ~EVENTFLAG_LEFT_MOUSE_BUTTON;
                ProcessMouseButton(static_cast<uint16_t>(position.x), static_cast<uint16_t>(position.y), MBT_LEFT, false);
            }

            if (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN)
            {
                s_overlayMouseButtonModifiers |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
                ProcessMouseButton(static_cast<uint16_t>(position.x), static_cast<uint16_t>(position.y), MBT_RIGHT, true);
            }

            if (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)
            {
                s_overlayMouseButtonModifiers &= ~EVENTFLAG_RIGHT_MOUSE_BUTTON;
                ProcessMouseButton(static_cast<uint16_t>(position.x), static_cast<uint16_t>(position.y), MBT_RIGHT, false);
            }

            if (mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN)
            {
                s_overlayMouseButtonModifiers |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
                ProcessMouseButton(static_cast<uint16_t>(position.x), static_cast<uint16_t>(position.y), MBT_MIDDLE, true);
            }

            if (mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP)
            {
                s_overlayMouseButtonModifiers &= ~EVENTFLAG_MIDDLE_MOUSE_BUTTON;
                ProcessMouseButton(static_cast<uint16_t>(position.x), static_cast<uint16_t>(position.y), MBT_MIDDLE, false);
            }
        }
    }
    else if (uMsg == WM_CHAR)
    {
        uint16_t scancode = (lParam >> 16) & 0xFF;
        uint16_t virtualKey = static_cast<uint16_t>(wParam);
        if (!IsWindowUnicode(hwnd))
        {
            wchar_t wch;
            ::MultiByteToWideChar(s_currentACP, MB_PRECOMPOSED, reinterpret_cast<char*>(&virtualKey), 2, &wch, sizeof(wchar_t));
            virtualKey = wch;
        }
        ProcessKeyboard(virtualKey, scancode, KEYEVENT_CHAR, false, false);
    }
    else if (uMsg == WM_INPUTLANGCHANGE)
    {
        s_currentACP = GetRealACP();
        spdlog::info("Input language changed, current ACP: {}", s_currentACP);
    }

    // While our UI is active it owns input. Do not let the same keyboard or
    // mouse event activate the native Skyrim menu underneath the modal panel.
    if (active && (uMsg == WM_INPUT || uMsg == WM_CHAR ||
        (uMsg >= WM_KEYFIRST && uMsg <= WM_KEYLAST) ||
        (uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST)))
        return 1;

    return 0;
}

InputService::InputService(OverlayService& aOverlay) noexcept
{
    s_pOverlay = &aOverlay;
    s_currentACP = GetRealACP();
}

InputService::~InputService() noexcept
{
    s_pOverlay = nullptr;
}
