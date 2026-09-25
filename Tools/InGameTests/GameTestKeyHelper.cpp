#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdlib>
#include <cstdio>
#include <cwchar>
#include <string>

// Test-only desktop actuator. Skyrim's native menu ignores input synthesized
// from its own process, but accepts the same event from an interactive peer.
// The bridge invokes this only after validating the visible RaceSex Menu.
int wmain(int argc, wchar_t** argv)
{
    if (argc != 4 && argc != 5 && argc != 6)
        return 2;

    wchar_t* end = nullptr;
    const auto rawWindow = std::wcstoull(argv[1], &end, 10);
    if (!end || *end != L'\0' || !rawWindow)
        return 3;
    end = nullptr;
    const auto rawProcess = std::wcstoul(argv[2], &end, 10);
    if (!end || *end != L'\0' || !rawProcess)
        return 4;

    const auto window = reinterpret_cast<HWND>(static_cast<uintptr_t>(rawWindow));
    DWORD windowProcess = 0;
    GetWindowThreadProcessId(window, &windowProcess);
    if (windowProcess != rawProcess || !IsWindowVisible(window) || GetForegroundWindow() != window)
        return 5;
    const auto targetFocused = [window, rawProcess]()
    {
        DWORD currentProcess = 0;
        GetWindowThreadProcessId(window, &currentProcess);
        return currentProcess == rawProcess && IsWindowVisible(window) &&
            GetForegroundWindow() == window;
    };

    // The game invokes this helper from its hooked WndProc and waits for the
    // helper to exit. Queueing both mouse edges before that handler returns
    // can lose the click. Fork an actuator, return to the window loop, then
    // wait for a fresh window-message round trip before sending input.
    if (std::wcsncmp(argv[3], L"run_", 4) != 0)
    {
        wchar_t self[MAX_PATH]{};
        const auto length = GetModuleFileNameW(nullptr, self, MAX_PATH);
        if (!length || length >= MAX_PATH)
            return 8;
        std::wstring commandLine = L"\"" + std::wstring(self) + L"\" " +
            argv[1] + L" " + argv[2] + L" run_" + argv[3];
        for (int index = 4; index < argc; ++index)
            commandLine += L" \"" + std::wstring(argv[index]) + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(self, commandLine.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
            return 8;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 0;
    }
    const wchar_t* action = argv[3] + 4;
    const auto trace = [action](const char* stage)
    {
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, path, MAX_PATH))
            return;
        std::wstring logPath(path);
        logPath += L".log";
        const HANDLE log = CreateFileW(logPath.c_str(), FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log == INVALID_HANDLE_VALUE)
            return;
        char line[160]{};
        const auto count = snprintf(line, sizeof(line), "%lu %ls %s\r\n",
            GetTickCount(), action, stage);
        if (count > 0)
        {
            DWORD written = 0;
            WriteFile(log, line, static_cast<DWORD>(count), &written, nullptr);
        }
        CloseHandle(log);
    };
    trace("child-start");
    const std::wstring mutexName = L"Local\\SkyrimSeamlessRaceTest." +
        std::to_wstring(rawProcess);
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!mutex)
        return 9;
    struct ScopedMutex
    {
        HANDLE Handle;
        bool Owned;
        ~ScopedMutex()
        {
            if (Owned)
                ReleaseMutex(Handle);
            CloseHandle(Handle);
        }
    } scopedMutex{mutex, false};
    // A second action is stale until the caller has observed the first UI
    // transition. Refuse overlap instead of queueing it behind an old state.
    const auto wait = WaitForSingleObject(mutex, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
    {
        trace("mutex-busy");
        return 9;
    }
    scopedMutex.Owned = true;
    if (!targetFocused())
    {
        trace("focus-lost-before-handshake");
        return 5;
    }
    DWORD_PTR ignored = 0;
    if (!SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG,
        2000, &ignored))
    {
        trace("wm-null-timeout");
        return 9;
    }
    if (!targetFocused())
    {
        trace("focus-lost-after-handshake");
        return 5;
    }
    trace("ready");

    if (std::wcscmp(action, L"click") == 0)
    {
        if (argc != 6)
            return 6;
        end = nullptr;
        const long x = std::wcstol(argv[4], &end, 10);
        if (!end || *end != L'\0')
            return 6;
        end = nullptr;
        const long y = std::wcstol(argv[5], &end, 10);
        if (!end || *end != L'\0')
            return 6;
        RECT client{};
        if (!GetClientRect(window, &client) || x < 0 || y < 0 ||
            x >= client.right || y >= client.bottom)
            return 7;
        POINT point{static_cast<LONG>(x), static_cast<LONG>(y)};
        if (!ClientToScreen(window, &point) || !SetCursorPos(point.x, point.y))
            return 7;
        Sleep(100);
        if (!targetFocused())
            return 5;
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        Sleep(120);
        if (!targetFocused())
        {
            mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            return 5;
        }
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
        trace("click-sent");
        return 0;
    }

    if (std::wcscmp(action, L"type") == 0)
    {
        if (argc != 5 || std::wcslen(argv[4]) > 32)
            return 6;
        for (const wchar_t* character = argv[4]; *character; ++character)
        {
            if (!((*character >= L'A' && *character <= L'Z') ||
                  (*character >= L'a' && *character <= L'z') ||
                  (*character >= L'0' && *character <= L'9') ||
                  *character == L' '))
                return 6;
        }
        for (const wchar_t* character = argv[4]; *character; ++character)
        {
            if (!targetFocused())
                return 5;
            const SHORT mapped = VkKeyScanW(*character);
            if (mapped == -1)
                return 7;
            const BYTE key = LOBYTE(mapped);
            const BYTE scan = static_cast<BYTE>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC));
            if (!scan)
                return 7;
            const bool shift = (HIBYTE(mapped) & 1) != 0;
            if (shift)
                keybd_event(VK_SHIFT, static_cast<BYTE>(MapVirtualKeyW(VK_SHIFT, MAPVK_VK_TO_VSC)), 0, 0);
            keybd_event(key, scan, 0, 0);
            Sleep(60);
            keybd_event(key, scan, KEYEVENTF_KEYUP, 0);
            if (shift)
                keybd_event(VK_SHIFT, static_cast<BYTE>(MapVirtualKeyW(VK_SHIFT, MAPVK_VK_TO_VSC)), KEYEVENTF_KEYUP, 0);
            Sleep(60);
        }
        trace("type-sent");
        return 0;
    }
    if (argc != 4)
        return 6;

    BYTE key = 0;
    if (std::wcscmp(action, L"done") == 0)
        key = 'R';
    else if (std::wcscmp(action, L"confirm") == 0)
        key = VK_RETURN;
    else if (std::wcscmp(action, L"left") == 0)
        key = VK_LEFT;
    else if (std::wcscmp(action, L"right") == 0)
        key = VK_RIGHT;
    else if (std::wcscmp(action, L"tab") == 0)
        key = VK_TAB;
    else if (std::wcscmp(action, L"space") == 0)
        key = VK_SPACE;
    else if (std::wcscmp(action, L"activate") == 0)
        key = 'E';
    else if (std::wcscmp(action, L"quicksave") == 0)
        key = VK_F5;
    else if (std::wcscmp(action, L"forward") == 0)
        key = 'W';
    else
        return 6;

    const auto scan = static_cast<BYTE>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC));
    if (!scan)
        return 7;

    if (std::wcscmp(action, L"forward") == 0)
    {
        // Trial a scan-code event against Skyrim's DirectInput poll path. The
        // bridge must verify the child log and game input counters separately;
        // successful SendInput alone does not prove the game consumed W.
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = scan;
        down.ki.dwFlags = KEYEVENTF_SCANCODE;
        INPUT up = down;
        up.ki.dwFlags |= KEYEVENTF_KEYUP;
        if (!targetFocused())
        {
            trace("forward-focus-lost-before-down");
            return 5;
        }
        if (SendInput(1, &down, sizeof(INPUT)) != 1)
        {
            trace("forward-down-failed");
            return 10;
        }
        trace("forward-down-sent");
        bool focusHeld = true;
        for (int elapsedMs = 0; elapsedMs < 350; elapsedMs += 10)
        {
            Sleep(10);
            if (!targetFocused())
            {
                focusHeld = false;
                break;
            }
        }
        // An up edge is required even if focus was lost. Retry it before
        // returning so a transient injection failure cannot strand W down.
        bool released = false;
        for (int attempt = 0; attempt < 5 && !released; ++attempt)
        {
            released = SendInput(1, &up, sizeof(INPUT)) == 1;
            if (!released)
                Sleep(10);
        }
        if (!released)
            keybd_event(key, scan, KEYEVENTF_KEYUP, 0);
        trace(!released ? "forward-release-failed-fallback-sent" :
            (focusHeld ? "forward-scan-sent" : "forward-focus-lost-released"));
        return released && focusHeld ? 0 : 10;
    }

    keybd_event(key, scan, 0, 0);
    Sleep(160);
    keybd_event(key, scan, KEYEVENTF_KEYUP, 0);
    trace("key-sent");
    return 0;
}
