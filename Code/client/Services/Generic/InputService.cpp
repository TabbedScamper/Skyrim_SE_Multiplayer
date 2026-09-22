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

static OverlayService* s_pOverlay = nullptr;
static UINT s_currentACP = CP_ACP;
static LONG s_overlayMouseX = 0;
static LONG s_overlayMouseY = 0;
static bool s_overlayMouseInitialized = false;
static uint16_t s_overlayMouseWidth = 0;
static uint16_t s_overlayMouseHeight = 0;
static uint32_t s_overlayMouseButtonModifiers = 0;
static bool s_systemCursorReleased = false;
static std::atomic_uint64_t s_lastControllerInputMs{0};

void InputService::NotifyControllerInput() noexcept
{
    s_lastControllerInputMs.store(GetTickCount64(), std::memory_order_relaxed);
}

void ReleaseCursorToWindows()
{
    ClipCursor(nullptr);
    ReleaseCapture();
    while (ShowCursor(TRUE) < 0)
        ;
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    s_systemCursorReleased = true;
}

void ReclaimCursorForGame()
{
    s_systemCursorReleased = false;
    while (ShowCursor(FALSE) >= 0)
        ;
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
    TiltedPhoques::DInputHook::Get().SetEnabled(aActive);
    aOverlay.SetActive(aActive);

    // Ensures the UI receives the current shell state if the initial event was sent too early.
    aOverlay.SetVersion(BUILD_COMMIT);
    aOverlay.GetOverlayApp()->ExecuteAsync(aOverlay.GetInGame() ? "enterGame" : "enterTitleScreen");

    // Skyrim's native Cursor Menu is the single cursor owner. It is rendered
    // over CEF by OverlayService, so never draw CEF's software cursor too.
    apRenderer->SetCursorVisible(false);

    // This is to disable the Windows cursor
    while (ShowCursor(FALSE) >= 0)
        ;
    s_systemCursorReleased = false;
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
    // Windows-key shell UI and Alt-Tab do not always deliver the initiating
    // key message to Skyrim. Focus loss is authoritative: immediately undo
    // Skyrim's ShowCursor/ClipCursor ownership so the desktop pointer remains
    // visible even while it crosses the unfocused game window.
    if (uMsg == WM_KILLFOCUS || (uMsg == WM_ACTIVATEAPP && wParam == FALSE) ||
        (uMsg == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE))
        ReleaseCursorToWindows();

    // Do not rely on having observed a particular deactivation message. The
    // foreground HWND is the source of truth whenever Windows asks which
    // cursor to display over this window.
    if (uMsg == WM_SETCURSOR && GetForegroundWindow() != hwnd)
    {
        ReleaseCursorToWindows();
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;
    }

    const auto pApp = s_pOverlay->GetOverlayApp();
    if (!pApp)
        return 0;

    const auto pClient = pApp->GetClient();
    if (!pClient)
        return 0;

    const auto pRenderer = pClient->GetOverlayRenderHandler();
    if (!pRenderer)
        return 0;

    // Win+Z belongs to the Windows shell. Skyrim normally keeps the shared
    // system cursor hidden and clipped even while the Snap Layout chooser is
    // visible, leaving the user unable to click a zone. Release it as soon as
    // either Windows key arrives and keep the arrow alive until focus or a
    // click explicitly returns to the game.
    if ((uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) &&
        (wParam == VK_LWIN || wParam == VK_RWIN))
        ReleaseCursorToWindows();

    if (s_systemCursorReleased && uMsg == WM_SETCURSOR)
    {
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return 1;
    }

    if (s_systemCursorReleased && uMsg == WM_LBUTTONDOWN)
        ReclaimCursorForGame();

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

    if (active)
    {
        position = GetOverlayMousePosition(pRenderer.get());
        if (s_pOverlay->GetTitleScreen())
            SetMainMenuMouseState(static_cast<float>(position.x), static_cast<float>(position.y));
    }

    ProcessMouseMove(static_cast<uint16_t>(position.x), static_cast<uint16_t>(position.y));

    if (uMsg == WM_INPUT)
    {
        RAWINPUT input;
        UINT size = sizeof(RAWINPUT);

        GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER));

        if (active)
        {
            auto& imgui = World::Get().ctx().at<ImguiService>();
            imgui.RawInputHandler(input);
        }

        if (input.header.dwType == RIM_TYPEKEYBOARD)
        {
            const auto keyboard = input.data.keyboard;

            if ((keyboard.Flags & RI_KEY_BREAK) == 0 &&
                (keyboard.VKey == VK_LWIN || keyboard.VKey == VK_RWIN))
                ReleaseCursorToWindows();

            ProcessKeyboard(keyboard.VKey, keyboard.MakeCode, keyboard.Flags & RI_KEY_BREAK ? KEYEVENT_KEYUP : KEYEVENT_KEYDOWN, keyboard.Flags & RI_KEY_E0, keyboard.Flags & RI_KEY_E1);
        }
        else if (input.header.dwType == RIM_TYPEMOUSE)
        {
            const auto mouse = input.data.mouse;

            if (active)
            {
                // Last-used-device behavior: physical mouse movement restores
                // the CEF pointer immediately; controller navigation hides it.
                // Window focus/warping can emit a final raw-mouse packet after a
                // controller action. Keep controller modality briefly; genuine
                // later mouse motion still restores the pointer immediately.
                if ((mouse.lLastX != 0 || mouse.lLastY != 0) &&
                    GetTickCount64() - s_lastControllerInputMs.load(std::memory_order_relaxed) >= 250)
                    pRenderer->SetCursorVisible(true);
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
    // If the player tabs out/in with UI visible, this WndProc doesn't run during mouse or keyboard events.
    // When player tabs in, force the UI state
    else if (uMsg == WM_SETFOCUS && s_pOverlay->GetActive())
    {
        TiltedPhoques::DInputHook::Get().SetEnabled(true);
        s_pOverlay->SetActive(true);
        pRenderer->SetCursorVisible(false);
        ReclaimCursorForGame();
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
