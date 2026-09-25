#include <TiltedOnlinePCH.h>

#include <Services/GameSettingsService.h>
#include <Services/OverlayService.h>
#include <World.h>

#include <Games/TES.h>
#include <Games/Skyrim/BSGraphics/BSGraphicsRenderer.h>
#include <Forms/TESForm.h>
#include <DefaultObjectManager.h>
#include <Games/Skyrim/Interface/MainMenuIntegration.h>
#include <Games/Skyrim/Audio/AudioDeviceSelection.h>
#include <Games/Skyrim/Audio/AudioPreview.h>
#include <Games/Skyrim/Interface/ControlBindings.h>
#include <OverlayApp.hpp>

#include <d3d11.h>
#include <fstream>

namespace
{
// BGSSoundCategory form IDs, read from Skyrim.esm SNCT records (EDID in
// brackets). These were previously rotated one slot, so each slider drove a
// different category than its label.
constexpr uint32_t cEffectsCategory = 0x000172A1;   // AudioCategorySFX
constexpr uint32_t cFootstepsCategory = 0x000F5FFC; // AudioCategoryFST
constexpr uint32_t cVoiceCategory = 0x000876BD;     // AudioCategoryVOCGeneral
constexpr uint32_t cMusicCategory = 0x00071E64;     // AudioCategoryMUS

float Clamp(float aValue, float aMinimum, float aMaximum)
{
    return std::max(aMinimum, std::min(aValue, aMaximum));
}

// Same call the vanilla Audio menu makes (OptionChange with the category form
// ID as option). Setting the category directly only stored the value: live
// sounds, including the menu music, kept their level.
void SetSoundCategoryVolume(uint32_t aCategoryFormId, float aValue)
{
    AudioPreview::SetCategoryVolumeVanilla(aCategoryFormId, Clamp(aValue, 0.f, 1.f));
}

// The native Journal Menu OptionChange handler has a dedicated path for the
// master volume (option 27). Category volume is not equivalent: the handler
// updates the active audio manager/mixer state in addition to the INI value.
void SetMasterVolume(float aValue)
{
    struct Callback
    {
        uint8_t Padding[0x28]{};
        void* Arguments{};
    } callback;
    struct Arguments
    {
        uint8_t Padding0[0x10]{};
        double OptionId{27.0};
        uint8_t Padding1[0x10]{};
        double Value{};
    } arguments;

    arguments.Value = static_cast<double>(Clamp(aValue, 0.f, 1.f));
    callback.Arguments = &arguments;
    static VersionDbPtr<void> s_optionChange(53310);
    using TOptionChange = void(void*);
    reinterpret_cast<TOptionChange*>(s_optionChange.GetPtr())(&callback);
}

float GetFloat(Setting* apSetting, float aFallback)
{
    return apSetting ? *reinterpret_cast<float*>(&apSetting->data) : aFallback;
}

bool GetBool(Setting* apSetting, bool aFallback)
{
    return apSetting ? (*reinterpret_cast<uint32_t*>(&apSetting->data) != 0) : aFallback;
}

float ReadIniFloat(const std::filesystem::path& acPath, const wchar_t* acSection, const wchar_t* acKey, float aFallback)
{
    const auto fallback = std::to_wstring(aFallback);
    wchar_t value[64]{};
    GetPrivateProfileStringW(acSection, acKey, fallback.c_str(), value, static_cast<DWORD>(std::size(value)), acPath.c_str());
    return static_cast<float>(_wtof(value));
}

struct MonitorDescription
{
    RECT Bounds{};
    std::wstring Name;
};

BOOL CALLBACK CollectMonitor(HMONITOR aMonitor, HDC, LPRECT, LPARAM aContext)
{
    auto& monitors = *reinterpret_cast<std::vector<MonitorDescription>*>(aContext);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(aMonitor, &info))
        monitors.push_back({info.rcMonitor, info.szDevice});
    return TRUE;
}

std::vector<MonitorDescription> GetMonitors()
{
    std::vector<MonitorDescription> monitors;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

// Index into GetMonitors() of the monitor showing most of the window.
int MonitorIndexOf(HWND aWindow)
{
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(MonitorFromWindow(aWindow, MONITOR_DEFAULTTONEAREST), &info))
        return 0;
    const auto monitors = GetMonitors();
    for (size_t i = 0; i < monitors.size(); ++i)
        if (monitors[i].Name == info.szDevice)
            return static_cast<int>(i);
    return 0;
}

// Where the framed window was last put, kept in [SkyrimTogether] of SkyrimPrefs.ini.
constexpr int cNoWindowOrigin = INT_MIN;

std::filesystem::path PrefsFile()
{
    wchar_t userProfile[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"USERPROFILE", userProfile, MAX_PATH))
        return std::filesystem::path(userProfile) / L"Documents" / L"My Games" / L"Skyrim Special Edition" / L"SkyrimPrefs.ini";
    return L"SkyrimPrefs.ini";
}

// A saved origin is reused only while the window's title bar would still be
// on a screen (a monitor may have been unplugged or rearranged since).
bool IsTitleBarOnScreen(int aX, int aY, int aOuterWidth)
{
    const RECT titleBar{aX + 40, aY, aX + std::max(aOuterWidth - 40, 80), aY + 24};
    return MonitorFromRect(&titleBar, MONITOR_DEFAULTTONULL) != nullptr;
}

bool IsPlausibleWindowedSize(const GameSettingsSnapshot& acSettings)
{
    const auto monitors = GetMonitors();
    if (monitors.empty())
        return acSettings.Width < 3840 && acSettings.Height < 2160;
    const auto index = std::clamp(acSettings.Monitor, 0, static_cast<int>(monitors.size()) - 1);
    const auto& bounds = monitors[index].Bounds;
    const int monitorWidth = bounds.right - bounds.left;
    const int monitorHeight = bounds.bottom - bounds.top;
    // A normal framed window must leave room in at least one dimension. This
    // rejects legacy values captured from a borderless/exclusive transition.
    return acSettings.Width < monitorWidth && acSettings.Height < monitorHeight;
}

std::string GetAudioValueKey(const std::filesystem::path& acPath, uint32_t aCategory)
{
    for (int index = 0; index < 8; ++index)
    {
        const auto key = L"uID" + std::to_wstring(index);
        if (GetPrivateProfileIntW(L"AudioMenu", key.c_str(), 0, acPath.c_str()) == aCategory)
            return "fVal" + std::to_string(index) + ":AudioMenu";
    }
    return {};
}

float ReadAudioValue(const std::filesystem::path& acPath, uint32_t aCategory, float aFallback)
{
    for (int index = 0; index < 8; ++index)
    {
        const auto idKey = L"uID" + std::to_wstring(index);
        if (GetPrivateProfileIntW(L"AudioMenu", idKey.c_str(), 0, acPath.c_str()) == aCategory)
        {
            const auto valueKey = L"fVal" + std::to_wstring(index);
            return ReadIniFloat(acPath, L"AudioMenu", valueKey.c_str(), aFallback);
        }
    }
    return aFallback;
}

void WriteInt(const std::filesystem::path& acPath, const wchar_t* acSection, const wchar_t* acKey, int aValue)
{
    const auto value = std::to_wstring(aValue);
    WritePrivateProfileStringW(acSection, acKey, value.c_str(), acPath.c_str());
}

void WriteFloat(const std::filesystem::path& acPath, const wchar_t* acSection, const wchar_t* acKey, float aValue)
{
    wchar_t value[32]{};
    swprintf_s(value, L"%.4f", aValue);
    WritePrivateProfileStringW(acSection, acKey, value, acPath.c_str());
}

void WriteAudioValue(const std::filesystem::path& acPath, uint32_t aCategory, float aValue)
{
    for (int index = 0; index < 8; ++index)
    {
        const auto idKey = L"uID" + std::to_wstring(index);
        if (GetPrivateProfileIntW(L"AudioMenu", idKey.c_str(), 0, acPath.c_str()) == aCategory)
        {
            const auto valueKey = L"fVal" + std::to_wstring(index);
            WriteFloat(acPath, L"AudioMenu", valueKey.c_str(), aValue);
            return;
        }
    }
}

std::filesystem::path CaptureFeedbackScreenshot()
{
    const auto* pWindow = BSGraphics::GetMainWindow();
    if (!pWindow || !pWindow->hWnd)
        return {};

    RECT client{};
    if (!GetClientRect(pWindow->hWnd, &client))
        return {};

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0)
        return {};

    HDC pWindowDc = GetDC(pWindow->hWnd);
    HDC pMemoryDc = pWindowDc ? CreateCompatibleDC(pWindowDc) : nullptr;
    HBITMAP pBitmap = pMemoryDc ? CreateCompatibleBitmap(pWindowDc, width, height) : nullptr;
    if (!pWindowDc || !pMemoryDc || !pBitmap)
    {
        if (pBitmap) DeleteObject(pBitmap);
        if (pMemoryDc) DeleteDC(pMemoryDc);
        if (pWindowDc) ReleaseDC(pWindow->hWnd, pWindowDc);
        return {};
    }

    HGDIOBJ pOldBitmap = SelectObject(pMemoryDc, pBitmap);
    BitBlt(pMemoryDc, 0, 0, width, height, pWindowDc, 0, 0, SRCCOPY | CAPTUREBLT);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    const bool copied = GetDIBits(pMemoryDc, pBitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS) != 0;

    SelectObject(pMemoryDc, pOldBitmap);
    DeleteObject(pBitmap);
    DeleteDC(pMemoryDc);
    ReleaseDC(pWindow->hWnd, pWindowDc);
    if (!copied)
        return {};

    SYSTEMTIME now{};
    GetLocalTime(&now);
    const auto directory = TiltedPhoques::GetPath() / "debug-feedback";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const auto filename = fmt::format("feedback-{:04}{:02}{:02}-{:02}{:02}{:02}-{}.bmp",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId());
    const auto path = directory / filename;

    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(pixels.size());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    return output ? path : std::filesystem::path{};
}
}

GameSettingsService::GameSettingsService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    (void)aDispatcher;
}

std::filesystem::path GameSettingsService::CaptureTestScreenshot() const noexcept
{
    return CaptureFeedbackScreenshot();
}

void GameSettingsService::QueueRequestSettings() noexcept
{
    m_mainLoopTasks.Add([this]() { RequestSettings(); });
    WakeWindowThread();
}

void GameSettingsService::QueuePreviewSetting(const String& acName, const String& acValue) noexcept
{
    m_mainLoopTasks.Add([this, name = acName, value = acValue]() { PreviewSetting(name, value); });
    WakeWindowThread();
}

void GameSettingsService::QueueConfirmDisplaySettings() noexcept
{
    m_mainLoopTasks.Add([this]() { ConfirmDisplaySettings(); });
    WakeWindowThread();
}

void GameSettingsService::QueueApplySettings(const GameSettingsSnapshot& acSettings) noexcept
{
    m_mainLoopTasks.Add([this, settings = acSettings]() { ApplySettings(settings); });
    WakeWindowThread();
}

void GameSettingsService::QueueRevertSettings() noexcept
{
    m_mainLoopTasks.Add([this]() { RevertSettings(); });
    WakeWindowThread();
}

void GameSettingsService::QueueResetSettings(const std::string& acSection) noexcept
{
    m_mainLoopTasks.Add([this, acSection]() { ResetSettings(acSection); });
    WakeWindowThread();
}

namespace
{
void SendControlBindings(const std::string& acJson)
{
    auto arguments = CefListValue::Create();
    arguments->SetString(0, acJson);
    World::Get().GetOverlayService().GetOverlayApp()->ExecuteAsync("controlBindings", arguments);
}
} // namespace

void GameSettingsService::QueueRequestControlBindings() noexcept
{
    m_mainLoopTasks.Add([]() {
        ControlBindings::SetBindingsChangedCallback(&SendControlBindings);
        SendControlBindings(ControlBindings::BindingsJson());
    });
    WakeWindowThread();
}

void GameSettingsService::QueueStartControlCapture(const String& acEvent, int aDevice) noexcept
{
    m_mainLoopTasks.Add([event = std::string(acEvent.c_str()), aDevice]() {
        if (aDevice >= 0 && aDevice <= 2)
            ControlBindings::StartCapture(event, static_cast<ControlBindings::Device>(aDevice));
    });
    WakeWindowThread();
}

void GameSettingsService::QueueAudioPreviewKeepAlive(const String& acChannel) noexcept
{
    m_mainLoopTasks.Add([channel = std::string(acChannel.c_str())]() { AudioPreview::KeepAlive(channel); });
    WakeWindowThread();
}

void GameSettingsService::QueueAudioPreviewStop() noexcept
{
    m_mainLoopTasks.Add([]() { AudioPreview::Stop(); });
    WakeWindowThread();
}

void GameSettingsService::QueueCancelControlCapture() noexcept
{
    m_mainLoopTasks.Add([]() { ControlBindings::CancelCapture(); });
    WakeWindowThread();
}

void GameSettingsService::WakeWindowThread() noexcept
{
    if (auto* pWindow = BSGraphics::GetMainWindow(); pWindow && pWindow->hWnd)
        PostMessageW(pWindow->hWnd, cGameSettingsWakeMessage, 0, 0);
}

void GameSettingsService::RequestSettings() noexcept
{
    spdlog::info("Reading game settings for the options UI");
    m_baseline = ReadSettings();
    m_preview = m_baseline;
    if (m_preview.DisplayMode != 0)
        m_lastExpandedMode = m_preview.DisplayMode;
    else if (IsPlausibleWindowedSize(m_preview))
    {
        m_lastWindowedWidth = m_preview.Width;
        m_lastWindowedHeight = m_preview.Height;
    }
    m_hasBaseline = true;
    SendSettings(m_preview);
}

void GameSettingsService::PreviewSetting(const String& acName, const String& acValue) noexcept
{
    if (!m_hasBaseline)
        RequestSettings();

    try
    {
        const std::string name = acName.c_str();
        const std::string value = acValue.c_str();
        spdlog::info("Previewing game setting {}={}", name, value);
        bool display = false;
        if (name == "displayMode") { m_preview.DisplayMode = std::stoi(value); display = true; }
        else if (name == "monitor") { m_preview.Monitor = std::stoi(value); display = true; }
        else if (name == "resolution")
        {
            const auto separator = value.find('x');
            if (separator == std::string::npos)
                throw std::invalid_argument("resolution");
            m_preview.Width = std::stoi(value.substr(0, separator));
            m_preview.Height = std::stoi(value.substr(separator + 1));
            display = true;
        }
        else if (name == "width") { m_preview.Width = std::stoi(value); display = true; }
        else if (name == "height") { m_preview.Height = std::stoi(value); display = true; }
        else if (name == "vsync") m_preview.VSync = value == "true" || value == "1";
        else if (name == "master") m_preview.MasterVolume = std::stof(value);
        else if (name == "footsteps") m_preview.FootstepsVolume = std::stof(value);
        else if (name == "voice") m_preview.VoiceVolume = std::stof(value);
        else if (name == "music") m_preview.MusicVolume = std::stof(value);
        else if (name == "effects") m_preview.EffectsVolume = std::stof(value);
        else if (name == "gamma") m_preview.Gamma = std::stof(value);
        else if (name == "mouseSensitivity") m_preview.MouseSensitivity = std::stof(value);
        else if (name == "gamepadSensitivity") m_preview.GamepadSensitivity = std::stof(value);
        else if (name == "invertY") m_preview.InvertY = value == "true" || value == "1";
        else if (name == "dialogueSubtitles") m_preview.DialogueSubtitles = value == "true" || value == "1";
        else if (name == "generalSubtitles") m_preview.GeneralSubtitles = value == "true" || value == "1";
        else if (name == "alwaysRun") m_preview.AlwaysRun = value == "true" || value == "1";
        else if (name == "controllerRumble") m_preview.ControllerRumble = value == "true" || value == "1";
        else if (name == "audioDevice") m_preview.AudioDevice = value;

        ApplyRuntime(m_preview, display);
        // Let the player hear the channel at its new level, alone.
        if (name == "master" || name == "effects" || name == "footsteps" || name == "voice" || name == "music")
            AudioPreview::KeepAlive(name);
        if (display)
        {
            m_displayPreviewActive = true;
            m_displayRevertDeadline = std::chrono::steady_clock::now() + 15s;
            if (auto* pWindow = BSGraphics::GetMainWindow(); pWindow && pWindow->hWnd)
                SetTimer(pWindow->hWnd, cGameSettingsTimerId, 50, nullptr);
            m_world.GetOverlayService().GetOverlayApp()->ExecuteAsync("displayPreviewStarted");
        }
        else
        {
            // Non-display controls are live settings and no longer need a
            // global Apply button. Save them immediately without accidentally
            // committing a display preview that is still awaiting approval.
            auto saved = m_preview;
            saved.DisplayMode = m_baseline.DisplayMode;
            saved.Monitor = m_baseline.Monitor;
            saved.Width = m_baseline.Width;
            saved.Height = m_baseline.Height;
            Persist(saved);
            m_baseline = saved;
        }
    }
    catch (const std::exception& exception)
    {
        spdlog::warn("Rejected invalid game setting {}={}: {}", acName, acValue, exception.what());
    }
}

void GameSettingsService::ConfirmDisplaySettings() noexcept
{
    if (!m_hasBaseline || !m_displayPreviewActive)
        return;

    // The selected mode is already live. Confirmation only persists that
    // exact preview; rebuilding the renderer here was the stale second resize
    // that could blank the title menu.
    Persist(m_preview);
    m_baseline = m_preview;
    m_displayPreviewActive = false;
    if (auto* pWindow = BSGraphics::GetMainWindow(); pWindow && pWindow->hWnd)
        KillTimer(pWindow->hWnd, cGameSettingsTimerId);
    m_world.GetOverlayService().GetOverlayApp()->ExecuteAsync("gameSettingsApplied");
    spdlog::info("Confirmed live display settings without a second resize");
}

void GameSettingsService::ApplySettings(const GameSettingsSnapshot& acSettings) noexcept
{
    spdlog::info("Applying options: mode={} monitor={} size={}x{} vsync={}", acSettings.DisplayMode,
        acSettings.Monitor, acSettings.Width, acSettings.Height, acSettings.VSync);
    m_preview = acSettings;
    m_preview.DisplayMode = std::clamp(m_preview.DisplayMode, 0, 2);
    m_preview.Monitor = std::max(m_preview.Monitor, 0);
    m_preview.Width = std::clamp(m_preview.Width, 640, 16384);
    m_preview.Height = std::clamp(m_preview.Height, 480, 16384);
    m_preview.MasterVolume = Clamp(m_preview.MasterVolume, 0.f, 1.f);
    m_preview.FootstepsVolume = Clamp(m_preview.FootstepsVolume, 0.f, 1.f);
    m_preview.VoiceVolume = Clamp(m_preview.VoiceVolume, 0.f, 1.f);
    m_preview.MusicVolume = Clamp(m_preview.MusicVolume, 0.f, 1.f);
    m_preview.EffectsVolume = Clamp(m_preview.EffectsVolume, 0.f, 1.f);
    m_preview.Gamma = Clamp(m_preview.Gamma, 0.5f, 1.5f);
    m_preview.MouseSensitivity = Clamp(m_preview.MouseSensitivity, 0.001f, 0.1f);
    m_preview.GamepadSensitivity = Clamp(m_preview.GamepadSensitivity, 0.1f, 2.f);
    ApplyRuntime(m_preview, true);
    Persist(m_preview);
    m_baseline = m_preview;
    m_hasBaseline = true;
    m_displayPreviewActive = false;
    m_world.GetOverlayService().GetOverlayApp()->ExecuteAsync("gameSettingsApplied");
}

void GameSettingsService::RevertSettings() noexcept
{
    if (!m_hasBaseline)
        return;
    m_preview = m_baseline;
    ApplyRuntime(m_baseline, true);
    m_displayPreviewActive = false;
    SendSettings(m_preview);
}

void GameSettingsService::ResetSettings(const std::string& acSection) noexcept
{
    // Only the named section's values return to their defaults; everything else
    // (and always the display mode, monitor and size) keeps its current value.
    const GameSettingsSnapshot defaults{};
    GameSettingsSnapshot result = m_hasBaseline ? m_baseline : ReadSettings();
    const bool all = acSection.empty();
    if (all || acSection == "display")
    {
        result.VSync = defaults.VSync;
        result.Gamma = defaults.Gamma;
    }
    if (all || acSection == "audio")
    {
        result.MasterVolume = defaults.MasterVolume;
        result.FootstepsVolume = defaults.FootstepsVolume;
        result.VoiceVolume = defaults.VoiceVolume;
        result.MusicVolume = defaults.MusicVolume;
        result.EffectsVolume = defaults.EffectsVolume;
        result.AudioDevice = defaults.AudioDevice;
    }
    if (all || acSection == "controls")
    {
        result.MouseSensitivity = defaults.MouseSensitivity;
        result.GamepadSensitivity = defaults.GamepadSensitivity;
        result.InvertY = defaults.InvertY;
        result.AlwaysRun = defaults.AlwaysRun;
        result.ControllerRumble = defaults.ControllerRumble;
        ControlBindings::ResetToDefaults();
    }
    if (all || acSection == "accessibility")
    {
        result.DialogueSubtitles = defaults.DialogueSubtitles;
        result.GeneralSubtitles = defaults.GeneralSubtitles;
    }
    spdlog::info("Restored defaults for {}", all ? "all game settings" : acSection);
    m_preview = result;
    ApplyRuntime(m_preview, false);
    Persist(m_preview);
    m_baseline = m_preview;
    m_hasBaseline = true;
    SendSettings(m_preview, true);
}

void GameSettingsService::ToggleWindowMode() noexcept
{
    auto settings = ReadSettings();
    spdlog::info("F11 display toggle requested from mode {}", settings.DisplayMode);
    if (settings.DisplayMode == 0)
    {
        // Preserve the actual framed-window client size before expanding. A
        // borderless window must persist the monitor dimensions; saving the
        // small window size with bBorderless=1 creates a frameless 1600x900
        // popup on the next launch.
        if (IsPlausibleWindowedSize(settings))
        {
            m_lastWindowedWidth = settings.Width;
            m_lastWindowedHeight = settings.Height;
        }
        settings.DisplayMode = m_lastExpandedMode == 2 ? 2 : 1;
        if (settings.DisplayMode == 1)
        {
            const auto monitors = GetMonitors();
            if (!monitors.empty())
            {
                const auto monitorIndex = std::clamp(settings.Monitor, 0, static_cast<int>(monitors.size()) - 1);
                const auto& bounds = monitors[monitorIndex].Bounds;
                settings.Width = bounds.right - bounds.left;
                settings.Height = bounds.bottom - bounds.top;
            }
        }
    }
    else
    {
        m_lastExpandedMode = settings.DisplayMode;
        settings.DisplayMode = 0;
        settings.Width = m_lastWindowedWidth;
        settings.Height = m_lastWindowedHeight;
    }

    ApplyRuntime(settings, true);
    Persist(settings);
    m_baseline = settings;
    m_preview = settings;
    m_hasBaseline = true;
    m_displayPreviewActive = false;
    SendSettings(settings);
    m_world.GetOverlayService().SendSystemMessage(
        settings.DisplayMode == 0 ? "Display mode: windowed" :
        (settings.DisplayMode == 1 ? "Display mode: borderless" : "Display mode: fullscreen"));

}

void GameSettingsService::RecordDebugFeedback(bool aLooksRight, const String& acNote) noexcept
{
    if (aLooksRight)
    {
        spdlog::info("In-game visual feedback: looks correct");
        return;
    }

    const auto screenshot = CaptureFeedbackScreenshot();
    if (screenshot.empty())
    {
        spdlog::error("In-game visual feedback screenshot failed");
        return;
    }

    RECT client{};
    const auto* pWindow = BSGraphics::GetMainWindow();
    if (pWindow && pWindow->hWnd)
        GetClientRect(pWindow->hWnd, &client);
    const auto* pRenderer = BSGraphics::GetRendererData();
    const auto notePath = screenshot.parent_path() / (screenshot.stem().string() + ".txt");
    std::ofstream note(notePath);
    note << "looksRight=false\n";
    note << "note=" << acNote.c_str() << "\n";
    note << "client=" << (client.right - client.left) << "x" << (client.bottom - client.top) << "\n";
    if (pRenderer)
        note << "rendererWindow=" << pRenderer->RenderWindowA[0].uiWindowWidth << "x"
             << pRenderer->RenderWindowA[0].uiWindowHeight << "\n";
    const auto gameStatePath = screenshot.parent_path() / (screenshot.stem().string() + ".game.json");
    std::ofstream gameState(gameStatePath, std::ios::binary);
    gameState << m_world.GetGameTestService().GetCachedGameSnapshot();
    spdlog::info("In-game visual feedback saved to {}", screenshot.string());
}

void GameSettingsService::OnMainLoop() noexcept
{
    // The Skyrim main loop continues on the title screen even though its VM
    // update (and therefore World::Update/RunnerService) is inactive.
    m_mainLoopTasks.Drain();

    if (m_pendingFullResize)
    {
        if (m_pendingResizeUpdates > 0)
        {
            --m_pendingResizeUpdates;
        }
        else
        {
            m_pendingFullResize = false;
            if (auto* pRenderer = BSGraphics::GetRenderer())
            {
                spdlog::info("Running deferred full renderer target rebuild");
                if (m_pendingEngineModeTransition)
                {
                    // WindowSizeChanged only observes the HWND client area. It
                    // cannot enter/leave DXGI exclusive fullscreen or select a
                    // different exclusive display mode. Use Skyrim's complete
                    // renderer transition for that case and retain the proven
                    // HWND resize path for ordinary windowed/borderless edits.
                    pRenderer->ResizeWindow(0, m_pendingRenderWidth, m_pendingRenderHeight,
                        m_pendingFullscreen, m_pendingBorderless);
                    if (!m_pendingFullscreen && !m_pendingBorderless)
                    {
                        // Leaving DXGI exclusive mode does not reliably restore
                        // Skyrim's framed HWND. Reapply the frame after the
                        // swap-chain transition so F11 produces a real movable
                        // window, not a full-monitor borderless-looking client.
                        if (auto* pWindow = BSGraphics::GetMainWindow(); pWindow && pWindow->hWnd)
                        {
                            MONITORINFO monitorInfo{};
                            monitorInfo.cbSize = sizeof(monitorInfo);
                            GetMonitorInfoW(MonitorFromWindow(pWindow->hWnd, MONITOR_DEFAULTTONEAREST), &monitorInfo);
                            RECT windowRect{0, 0, static_cast<LONG>(m_pendingRenderWidth),
                                static_cast<LONG>(m_pendingRenderHeight)};
                            AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);
                            const int outerWidth = windowRect.right - windowRect.left;
                            const int outerHeight = windowRect.bottom - windowRect.top;
                            const auto origin = WindowedOrigin(monitorInfo.rcWork, outerWidth, outerHeight);
                            const int x = origin.x;
                            const int y = origin.y;
                            SetWindowLongPtrW(pWindow->hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
                            SetWindowPos(pWindow->hWnd, HWND_TOP, x, y, outerWidth, outerHeight,
                                SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                            pRenderer->WindowSizeChanged(0);
                        }
                    }
                    m_pendingEngineModeTransition = false;
                }
                else
                {
                    pRenderer->WindowSizeChanged(0);
                }
                RefreshMainMenuLayout();
                spdlog::info("Deferred full renderer target rebuild completed");
            }
            // Keep ignoring transition-generated WM_SIZE messages for two
            // additional updates; DXGI can post one after ResizeWindow returns.
            m_resizeEventGuardUpdates = 2;
        }
    }

    if (!m_pendingFullResize && m_programmaticDisplayChange)
    {
        if (m_resizeEventGuardUpdates > 0)
            --m_resizeEventGuardUpdates;
        else
            m_programmaticDisplayChange = false;
    }

    if (m_displayPreviewActive && std::chrono::steady_clock::now() >= m_displayRevertDeadline)
    {
        spdlog::warn("Display preview timed out; restoring previous settings");
        RevertSettings();
        m_world.GetOverlayService().GetOverlayApp()->ExecuteAsync("displayPreviewReverted");
    }

    // The timer is the only thing that runs this, so it must outlive the
    // resize guard too: stopping it early left m_programmaticDisplayChange set
    // for the rest of the session, and every later move or resize by the
    // player was ignored.
    if (!m_pendingFullResize && !m_displayPreviewActive && !m_programmaticDisplayChange)
        if (auto* pWindow = BSGraphics::GetMainWindow(); pWindow && pWindow->hWnd)
            KillTimer(pWindow->hWnd, cGameSettingsTimerId);
}

void GameSettingsService::OnWindowSizeChanged(WPARAM aSizeType) noexcept
{
    if (m_programmaticDisplayChange || aSizeType == SIZE_MINIMIZED)
        return;

    auto* pWindow = BSGraphics::GetMainWindow();
    auto* pRenderer = BSGraphics::GetRendererData();
    if (!pWindow || !pWindow->hWnd || !pRenderer)
        return;

    const auto style = static_cast<DWORD>(GetWindowLongPtrW(pWindow->hWnd, GWL_STYLE));
    if ((style & WS_OVERLAPPEDWINDOW) != WS_OVERLAPPEDWINDOW)
        return;

    RECT client{};
    if (!GetClientRect(pWindow->hWnd, &client))
        return;
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width < 330 || height < 240)
        return;

    if (!m_hasBaseline)
    {
        m_baseline = ReadSettings();
        m_preview = m_baseline;
        m_hasBaseline = true;
    }

    m_lastWindowedWidth = width;
    m_lastWindowedHeight = height;
    m_baseline.DisplayMode = 0;
    m_baseline.Width = width;
    m_baseline.Height = height;
    m_preview.DisplayMode = 0;
    m_preview.Width = width;
    m_preview.Height = height;
    m_displayPreviewActive = false;
    pRenderer->bAppFullScreen = false;
    pRenderer->bBorderlessWindow = false;

    const auto path = GetPrefsPath();
    WriteInt(path, L"Display", L"bFull Screen", 0);
    WriteInt(path, L"Display", L"bBorderless", 0);
    WriteInt(path, L"Display", L"iSize W", width);
    WriteInt(path, L"Display", L"iSize H", height);
    spdlog::info("Accepted Windows window resize/snap at {}x{}", width, height);
    RefreshMainMenuLayout();
    SendSettings(m_preview);
}

GameSettingsSnapshot GameSettingsService::ReadSettings() const noexcept
{
    GameSettingsSnapshot result{};
    const auto path = GetPrefsPath();
    const bool borderless = GetPrivateProfileIntW(L"Display", L"bBorderless", 1, path.c_str()) != 0;
    const bool fullscreen = GetPrivateProfileIntW(L"Display", L"bFull Screen", 0, path.c_str()) != 0;
    result.DisplayMode = borderless ? 1 : (fullscreen ? 2 : 0);
    result.Width = GetPrivateProfileIntW(L"Display", L"iSize W", 1920, path.c_str());
    result.Height = GetPrivateProfileIntW(L"Display", L"iSize H", 1080, path.c_str());
    result.VSync = GetPrivateProfileIntW(L"Display", L"iVSyncPresentInterval", 1, path.c_str()) != 0;
    result.MasterVolume = ReadIniFloat(path, L"AudioMenu", L"fAudioMasterVolume", 1.f);
    result.FootstepsVolume = ReadAudioValue(path, cFootstepsCategory, 1.f);
    result.VoiceVolume = ReadAudioValue(path, cVoiceCategory, 1.f);
    result.MusicVolume = ReadAudioValue(path, cMusicCategory, 1.f);
    result.EffectsVolume = ReadAudioValue(path, cEffectsCategory, 1.f);
    result.Gamma = ReadIniFloat(path, L"Display", L"fGamma", 1.f);
    result.MouseSensitivity = ReadIniFloat(path, L"Controls", L"fMouseHeadingSensitivity", 0.0125f);
    result.GamepadSensitivity = ReadIniFloat(path, L"Controls", L"fGamepadHeadingSensitivity", 0.6667f);
    result.InvertY = GetPrivateProfileIntW(L"Controls", L"bInvertYValues", 0, path.c_str()) != 0;
    result.DialogueSubtitles = GetPrivateProfileIntW(L"Interface", L"bDialogueSubtitles", 1, path.c_str()) != 0;
    result.GeneralSubtitles = GetPrivateProfileIntW(L"Interface", L"bGeneralSubtitles", 1, path.c_str()) != 0;
    result.AlwaysRun = GetPrivateProfileIntW(L"Controls", L"bAlwaysRunByDefault", 1, path.c_str()) != 0;
    result.ControllerRumble = GetPrivateProfileIntW(L"Controls", L"bGamePadRumble", 1, path.c_str()) != 0;
    result.AudioDevice = AudioDeviceSelection::GetPreferredDevice();

    if (auto* pRenderer = BSGraphics::GetRendererData())
    {
        // Skyrim's renderer fields can contain the outer framed HWND size in
        // windowed mode. Feeding those values back through AdjustWindowRect
        // adds the border again on every F11 round trip (+16x39 at 100% DPI).
        // The user-facing windowed resolution is the client area.
        RECT client{};
        auto* pWindow = BSGraphics::GetMainWindow();
        if (result.DisplayMode == 0 && pWindow && pWindow->hWnd && GetClientRect(pWindow->hWnd, &client))
        {
            result.Width = client.right - client.left;
            result.Height = client.bottom - client.top;
        }
        else
        {
            result.Width = static_cast<int>(pRenderer->RenderWindowA[0].uiWindowWidth);
            result.Height = static_cast<int>(pRenderer->RenderWindowA[0].uiWindowHeight);
        }
        result.VSync = pRenderer->uiPresentInterval != 0;
    }
    // Borderless and fullscreen open on the monitor the game is on now.
    if (auto* pWindow = BSGraphics::GetMainWindow(); pWindow && pWindow->hWnd)
        result.Monitor = MonitorIndexOf(pWindow->hWnd);

    return result;
}

void GameSettingsService::ApplyRuntime(const GameSettingsSnapshot& acSettings, bool aDisplay) noexcept
{
    if (aDisplay)
        ApplyDisplay(acSettings);

    if (auto* pRenderer = BSGraphics::GetRendererData())
    {
        pRenderer->uiPresentInterval = acSettings.VSync ? 1 : 0;
    }

    SetFloatSetting("fAudioMasterVolume:AudioMenu", Clamp(acSettings.MasterVolume, 0.f, 1.f));
    SetMasterVolume(acSettings.MasterVolume);
    const auto path = GetPrefsPath();
    for (const auto& [category, value] : std::initializer_list<std::pair<uint32_t, float>>{
             {cFootstepsCategory, acSettings.FootstepsVolume}, {cVoiceCategory, acSettings.VoiceVolume},
             {cMusicCategory, acSettings.MusicVolume}, {cEffectsCategory, acSettings.EffectsVolume}})
    {
        const auto key = GetAudioValueKey(path, category);
        if (!key.empty())
            SetFloatSetting(key.c_str(), Clamp(value, 0.f, 1.f));
        SetSoundCategoryVolume(category, value);
    }
    // The playing music track does not pick up category changes by itself.
    AudioPreview::SyncMusicVolume();
    SetFloatSetting("fGamma:Display", Clamp(acSettings.Gamma, 0.5f, 1.5f));
    // Address Library 388988 is the live fGamma data used by the vanilla
    // Journal Menu's OptionChange case on 1.7.104. Writing it explicitly
    // mirrors the native brightness slider even if collection lookup resolves
    // a persisted wrapper rather than the renderer-facing storage.
    static VersionDbPtr<float> s_liveGamma(388988);
    if (auto* pGamma = s_liveGamma.Get())
        *pGamma = Clamp(acSettings.Gamma, 0.5f, 1.5f);
    SetFloatSetting("fMouseHeadingSensitivity:Controls", Clamp(acSettings.MouseSensitivity, 0.001f, 0.1f));
    SetFloatSetting("fGamepadHeadingSensitivity:Controls", Clamp(acSettings.GamepadSensitivity, 0.1f, 2.f));
    SetBoolSetting("bInvertYValues:Controls", acSettings.InvertY);
    SetBoolSetting("bDialogueSubtitles:Interface", acSettings.DialogueSubtitles);
    SetBoolSetting("bGeneralSubtitles:Interface", acSettings.GeneralSubtitles);
    SetBoolSetting("bAlwaysRunByDefault:Controls", acSettings.AlwaysRun);
    SetBoolSetting("bGamePadRumble:Controls", acSettings.ControllerRumble);
}

void GameSettingsService::ApplyDisplay(const GameSettingsSnapshot& acSettings) noexcept
{
    auto* pRenderer = BSGraphics::GetRendererData();
    auto* pWindow = BSGraphics::GetMainWindow();
    if (!pRenderer || !pWindow || !pWindow->hWnd)
        return;

    const int width = std::clamp(acSettings.Width, 640, 16384);
    const int height = std::clamp(acSettings.Height, 480, 16384);
    auto monitors = GetMonitors();
    if (monitors.empty())
        return;
    const auto monitorIndex = std::clamp(acSettings.Monitor, 0, static_cast<int>(monitors.size()) - 1);
    const auto bounds = monitors[monitorIndex].Bounds;

    m_programmaticDisplayChange = true;

    uint32_t renderWidth = width;
    uint32_t renderHeight = height;
    DXGI_SWAP_CHAIN_DESC swapDesc{};
    const bool haveSwapDesc = pWindow->pSwapChain && SUCCEEDED(pWindow->pSwapChain->GetDesc(&swapDesc));
    const bool engineModeTransition = acSettings.DisplayMode == 2 || (haveSwapDesc && !swapDesc.Windowed);

    if (!engineModeTransition && acSettings.DisplayMode == 1)
    {
        SetWindowLongPtrW(pWindow->hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(pWindow->hWnd, HWND_TOP, bounds.left, bounds.top, bounds.right - bounds.left,
            bounds.bottom - bounds.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        renderWidth = static_cast<uint32_t>(bounds.right - bounds.left);
        renderHeight = static_cast<uint32_t>(bounds.bottom - bounds.top);
    }
    else if (!engineModeTransition && acSettings.DisplayMode == 0)
    {
        RECT windowRect{0, 0, width, height};
        AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);
        const int outerWidth = windowRect.right - windowRect.left;
        const int outerHeight = windowRect.bottom - windowRect.top;
        const auto origin = WindowedOrigin(bounds, outerWidth, outerHeight);
        const int x = origin.x;
        const int y = origin.y;
        SetWindowLongPtrW(pWindow->hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(pWindow->hWnd, HWND_TOP, x, y, outerWidth, outerHeight, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }

    if (haveSwapDesc)
        spdlog::info("Display resize: swap chain {}x{} windowed={}, request {}x{} (mode {}, engine transition={})",
            swapDesc.BufferDesc.Width, swapDesc.BufferDesc.Height, swapDesc.Windowed, renderWidth, renderHeight,
            acSettings.DisplayMode, engineModeTransition);

    // The complete target rebuild is unsafe from the end-of-frame overlay
    // callback. Schedule Skyrim's supported WindowSizeChanged path at the next
    // game-update boundary, before another frame starts rendering.
    pRenderer->bAppFullScreen = acSettings.DisplayMode == 2;
    pRenderer->bBorderlessWindow = acSettings.DisplayMode == 1;
    pRenderer->bRequestWindowSizeChange = false;
    m_pendingEngineModeTransition = engineModeTransition;
    m_pendingRenderWidth = renderWidth;
    m_pendingRenderHeight = renderHeight;
    m_pendingFullscreen = acSettings.DisplayMode == 2;
    m_pendingBorderless = acSettings.DisplayMode == 1;
    m_pendingFullResize = true;
    m_pendingResizeUpdates = 1;
    m_resizeEventGuardUpdates = 0;
    SetTimer(pWindow->hWnd, cGameSettingsTimerId, 50, nullptr);

    // Changing WS_OVERLAPPEDWINDOW/WS_POPUP can make Windows briefly transfer
    // activation. Keep Skyrim presenting instead of leaving the last frame up.
    ShowWindow(pWindow->hWnd, SW_RESTORE);
    BringWindowToTop(pWindow->hWnd);
    SetForegroundWindow(pWindow->hWnd);
    SetFocus(pWindow->hWnd);
}

void GameSettingsService::Persist(const GameSettingsSnapshot& acSettings) const noexcept
{
    const auto path = GetPrefsPath();
    WriteInt(path, L"Display", L"bFull Screen", acSettings.DisplayMode == 2);
    WriteInt(path, L"Display", L"bBorderless", acSettings.DisplayMode == 1);
    WriteInt(path, L"Display", L"iSize W", acSettings.Width);
    WriteInt(path, L"Display", L"iSize H", acSettings.Height);
    WriteInt(path, L"Display", L"iVSyncPresentInterval", acSettings.VSync ? 1 : 0);
    WriteFloat(path, L"AudioMenu", L"fAudioMasterVolume", acSettings.MasterVolume);
    WriteAudioValue(path, cFootstepsCategory, acSettings.FootstepsVolume);
    WriteAudioValue(path, cVoiceCategory, acSettings.VoiceVolume);
    WriteAudioValue(path, cMusicCategory, acSettings.MusicVolume);
    WriteAudioValue(path, cEffectsCategory, acSettings.EffectsVolume);
    WriteFloat(path, L"Display", L"fGamma", acSettings.Gamma);
    WriteFloat(path, L"Controls", L"fMouseHeadingSensitivity", acSettings.MouseSensitivity);
    WriteFloat(path, L"Controls", L"fGamepadHeadingSensitivity", acSettings.GamepadSensitivity);
    WriteInt(path, L"Controls", L"bInvertYValues", acSettings.InvertY);
    WriteInt(path, L"Interface", L"bDialogueSubtitles", acSettings.DialogueSubtitles);
    WriteInt(path, L"Interface", L"bGeneralSubtitles", acSettings.GeneralSubtitles);
    WriteInt(path, L"Controls", L"bAlwaysRunByDefault", acSettings.AlwaysRun);
    WriteInt(path, L"Controls", L"bGamePadRumble", acSettings.ControllerRumble);
    if (acSettings.AudioDevice != AudioDeviceSelection::GetPreferredDevice())
    {
        AudioDeviceSelection::SetPreferredDevice(acSettings.AudioDevice);
        AudioDeviceSelection::ApplyPreferredDevice();
    }
}

void GameSettingsService::SendSettings(const GameSettingsSnapshot& acSettings, bool aDefaults) const noexcept
{
    auto arguments = CefListValue::Create();
    arguments->SetInt(0, acSettings.DisplayMode);
    arguments->SetInt(1, acSettings.Monitor);
    arguments->SetInt(2, acSettings.Width);
    arguments->SetInt(3, acSettings.Height);
    arguments->SetBool(4, acSettings.VSync);
    arguments->SetDouble(5, acSettings.MasterVolume);
    arguments->SetDouble(6, acSettings.FootstepsVolume);
    arguments->SetDouble(7, acSettings.VoiceVolume);
    arguments->SetDouble(8, acSettings.MusicVolume);
    arguments->SetDouble(9, acSettings.EffectsVolume);
    arguments->SetDouble(10, acSettings.Gamma);
    arguments->SetDouble(11, acSettings.MouseSensitivity);
    arguments->SetDouble(12, acSettings.GamepadSensitivity);
    arguments->SetBool(13, acSettings.InvertY);
    arguments->SetBool(14, acSettings.DialogueSubtitles);
    arguments->SetBool(15, acSettings.GeneralSubtitles);
    arguments->SetBool(16, acSettings.AlwaysRun);
    arguments->SetBool(17, acSettings.ControllerRumble);

    std::string monitorNames;
    for (const auto& monitor : GetMonitors())
    {
        if (!monitorNames.empty())
            monitorNames += '|';
        monitorNames += "Display " + std::to_string(std::count(monitorNames.begin(), monitorNames.end(), '|') + 1) + " (" +
            std::to_string(monitor.Bounds.right - monitor.Bounds.left) + "x" +
            std::to_string(monitor.Bounds.bottom - monitor.Bounds.top) + ")";
    }
    arguments->SetString(18, monitorNames);
    std::string resolutions = "1280x720|1600x900|1920x1080|2560x1080|2560x1440|3440x1440|3840x2160|5120x1440";
    const auto currentResolution = std::to_string(acSettings.Width) + "x" + std::to_string(acSettings.Height);
    if (resolutions.find(currentResolution) == std::string::npos)
        resolutions = currentResolution + '|' + resolutions;
    arguments->SetString(19, resolutions);
    arguments->SetBool(20, aDefaults);
    arguments->SetString(21, acSettings.AudioDevice);
    arguments->SetString(22, AudioDeviceSelection::OutputsJson());
    m_world.GetOverlayService().GetOverlayApp()->ExecuteAsync("gameSettings", arguments);
}

void GameSettingsService::SetFloatSetting(const char* acName, float aValue) const noexcept
{
    if (auto* pCollection = INISettingCollection::Get())
        if (auto* pSetting = pCollection->GetSetting(acName))
            *reinterpret_cast<float*>(&pSetting->data) = aValue;
}

void GameSettingsService::SetBoolSetting(const char* acName, bool aValue) const noexcept
{
    if (auto* pCollection = INISettingCollection::Get())
        if (auto* pSetting = pCollection->GetSetting(acName))
            *reinterpret_cast<uint32_t*>(&pSetting->data) = aValue ? 1u : 0u;
}

std::filesystem::path GameSettingsService::GetPrefsPath() const noexcept
{
    return PrefsFile();
}

bool GameSettingsService::SavedWindowedOrigin(int& aX, int& aY, int aOuterWidth) noexcept
{
    const auto path = PrefsFile();
    const int x = GetPrivateProfileIntW(L"SkyrimTogether", L"iWindowX", cNoWindowOrigin, path.c_str());
    const int y = GetPrivateProfileIntW(L"SkyrimTogether", L"iWindowY", cNoWindowOrigin, path.c_str());
    if (x == cNoWindowOrigin || y == cNoWindowOrigin || !IsTitleBarOnScreen(x, y, aOuterWidth))
        return false;
    aX = x;
    aY = y;
    return true;
}

POINT GameSettingsService::WindowedOrigin(const RECT& acBounds, int aOuterWidth, int aOuterHeight) const noexcept
{
    int x = 0, y = 0;
    if (SavedWindowedOrigin(x, y, aOuterWidth))
        return {x, y};
    return {acBounds.left + ((acBounds.right - acBounds.left) - aOuterWidth) / 2,
        acBounds.top + ((acBounds.bottom - acBounds.top) - aOuterHeight) / 2};
}

void GameSettingsService::OnWindowPlacementChanged(UINT aMessage) noexcept
{
    if (aMessage == WM_ENTERSIZEMOVE)
    {
        m_inSizeMove = true;
        return;
    }
    if (aMessage == WM_EXITSIZEMOVE)
        m_inSizeMove = false;
    else if (m_inSizeMove)
        return; // saved once the drag ends; WM_WINDOWPOSCHANGED alone covers keyboard snaps

    // Only where the player put a framed window: our own mode switches and
    // Skyrim's transitions are not a choice of position.
    auto* pWindow = BSGraphics::GetMainWindow();
    if (m_programmaticDisplayChange || m_pendingFullResize || !pWindow || !pWindow->hWnd)
        return;
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(pWindow->hWnd, GWL_STYLE));
    if ((style & WS_OVERLAPPEDWINDOW) != WS_OVERLAPPEDWINDOW || IsIconic(pWindow->hWnd) || IsZoomed(pWindow->hWnd))
        return;
    RECT rect{};
    if (!GetWindowRect(pWindow->hWnd, &rect) || (rect.left == m_savedWindowX && rect.top == m_savedWindowY))
        return;

    m_savedWindowX = rect.left;
    m_savedWindowY = rect.top;
    const auto path = GetPrefsPath();
    WriteInt(path, L"SkyrimTogether", L"iWindowX", rect.left);
    WriteInt(path, L"SkyrimTogether", L"iWindowY", rect.top);
    spdlog::info("Remembered windowed position {},{}", rect.left, rect.top);
}
