#pragma once

struct OverlayService;

/**
 * @brief Handles input handling for the UI.
 */
struct InputService
{
    InputService(OverlayService& aOverlay) noexcept;
    ~InputService() noexcept;

    static LRESULT WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    // Runs after Skyrim's own WndProc so its deactivation handling cannot win.
    static void AfterGameWndProc(HWND hwnd, UINT uMsg) noexcept;
    static void NotifyControllerInput() noexcept;
    // Safe from any thread: re-evaluates Windows pointer ownership on the window thread.
    static void RequestCursorUpdate() noexcept;

    // Posted to the game window by RequestCursorUpdate.
    static constexpr UINT cCursorUpdateMessage = WM_APP + 0x3C1;

    TP_NOCOPYMOVE(InputService);
};
