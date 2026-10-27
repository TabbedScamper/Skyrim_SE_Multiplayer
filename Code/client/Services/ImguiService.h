#pragma once

#include <imgui/ImGuiDriver.h>

struct RenderSystemD3D9;
struct RenderSystemD3D11;

/**
 * @brief Draws the ImGui UI.
 */
struct ImguiService
{
    using TCallback = void();

    ImguiService();
    ~ImguiService() noexcept;

    TP_NOCOPYMOVE(ImguiService);

    void Create(RenderSystemD3D11* apRenderSystem, HWND aHwnd);

    void Render() const;
    void DrawLastFrame() const;
    void Reset() const;

    LRESULT WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void RawInputHandler(RAWINPUT& aRawinput);
    // The overlay's pointer as a fraction of the screen (InputService), used as ImGui's mouse position while the
    // overlay is active; cleared when it is not.
    static void SetPointer(float aU, float aV) noexcept;
    static void ClearPointer() noexcept;

    entt::sink<entt::sigh<TCallback>> OnDraw;

private:
    ImGuiImpl::ImGuiDriver m_imDriver;
    entt::sigh<TCallback> m_drawSignal;
};
