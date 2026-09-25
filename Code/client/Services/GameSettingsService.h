#pragma once

#include <TiltedCore/TaskQueue.hpp>

struct World;

inline constexpr UINT cGameSettingsWakeMessage = WM_APP + 0x51A;
inline constexpr UINT_PTR cGameSettingsTimerId = 0x53534D50;

struct GameSettingsSnapshot
{
    int DisplayMode{1};
    int Monitor{0};
    int Width{1920};
    int Height{1080};
    bool VSync{true};
    float MasterVolume{1.f};
    float FootstepsVolume{1.f};
    float VoiceVolume{1.f};
    float MusicVolume{1.f};
    float EffectsVolume{1.f};
    float Gamma{1.f};
    float MouseSensitivity{0.0125f};
    float GamepadSensitivity{0.6667f};
    bool InvertY{false};
    bool DialogueSubtitles{true};
    bool GeneralSubtitles{true};
    bool AlwaysRun{true};
    bool ControllerRumble{true};
    std::string AudioDevice; // MMDevice endpoint ID; empty = Windows default
};

struct GameSettingsService
{
    GameSettingsService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~GameSettingsService() noexcept = default;

    TP_NOCOPYMOVE(GameSettingsService);

    void RequestSettings() noexcept;
    void PreviewSetting(const String& acName, const String& acValue) noexcept;
    void ConfirmDisplaySettings() noexcept;
    void ApplySettings(const GameSettingsSnapshot& acSettings) noexcept;
    void RevertSettings() noexcept;
    void ResetSettings() noexcept;
    void ToggleWindowMode() noexcept;
    void RecordDebugFeedback(bool aLooksRight, const String& acNote) noexcept;
    [[nodiscard]] std::filesystem::path CaptureTestScreenshot() const noexcept;

    // CEF callbacks run off the game thread, and Skyrim's VM update does not
    // run on the title screen. Queue settings work for the always-running
    // main-loop hook instead of the gameplay-only RunnerService.
    void QueueRequestSettings() noexcept;
    void QueuePreviewSetting(const String& acName, const String& acValue) noexcept;
    void QueueConfirmDisplaySettings() noexcept;
    void QueueApplySettings(const GameSettingsSnapshot& acSettings) noexcept;
    void QueueRevertSettings() noexcept;
    void QueueResetSettings() noexcept;
    // Key bindings (ControlBindings) and live audio previews.
    void QueueRequestControlBindings() noexcept;
    void QueueStartControlCapture(const String& acEvent, int aDevice) noexcept;
    void QueueCancelControlCapture() noexcept;
    void QueueAudioPreviewKeepAlive(const String& acChannel) noexcept;
    void QueueAudioPreviewStop() noexcept;
    void OnMainLoop() noexcept;
    void OnWindowSizeChanged(WPARAM aSizeType) noexcept;

private:
    void WakeWindowThread() noexcept;
    GameSettingsSnapshot ReadSettings() const noexcept;
    void ApplyRuntime(const GameSettingsSnapshot& acSettings, bool aDisplay) noexcept;
    void ApplyDisplay(const GameSettingsSnapshot& acSettings) noexcept;
    void Persist(const GameSettingsSnapshot& acSettings) const noexcept;
    void SendSettings(const GameSettingsSnapshot& acSettings, bool aDefaults = false) const noexcept;
    void SetFloatSetting(const char* acName, float aValue) const noexcept;
    void SetBoolSetting(const char* acName, bool aValue) const noexcept;
    std::filesystem::path GetPrefsPath() const noexcept;

    World& m_world;
    TiltedPhoques::TaskQueue m_mainLoopTasks;
    GameSettingsSnapshot m_baseline{};
    GameSettingsSnapshot m_preview{};
    bool m_hasBaseline{false};
    int m_lastExpandedMode{1};
    int m_lastWindowedWidth{1600};
    int m_lastWindowedHeight{900};
    bool m_displayPreviewActive{false};
    std::chrono::steady_clock::time_point m_displayRevertDeadline{};
    bool m_pendingFullResize{false};
    bool m_pendingEngineModeTransition{false};
    uint32_t m_pendingRenderWidth{0};
    uint32_t m_pendingRenderHeight{0};
    bool m_pendingFullscreen{false};
    bool m_pendingBorderless{false};
    uint32_t m_pendingResizeUpdates{0};
    uint32_t m_resizeEventGuardUpdates{0};
    bool m_programmaticDisplayChange{false};
    bool m_feedbackPromptPending{false};
    std::chrono::steady_clock::time_point m_feedbackPromptDeadline{};
};
