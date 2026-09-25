#include <TiltedOnlinePCH.h>

#include <Services/OverlayService.h>

#include <OverlayApp.hpp>

#include <D3D11Hook.hpp>
#include <DInputHook.hpp>
#include <OverlayRenderHandlerD3D11.hpp>

#include <Systems/RenderSystemD3D11.h>

#include <World.h>
#include <Games/Skyrim/BSGraphics/BSGraphicsRenderer.h>
#include <Games/Skyrim/Interface/MainMenuIntegration.h>
#include <Games/Skyrim/Interface/ControlBindings.h>

#include <Services/OverlayClient.h>
#include <Services/InputService.h>
#include <Services/TransportService.h>

#include <Messages/NotifyChatMessageBroadcast.h>
#include <Messages/NotifyPlayerList.h>
#include <Messages/NotifyPlayerLeft.h>
#include <Messages/NotifyPlayerJoined.h>
#include <Messages/NotifyPlayerDialogue.h>
#include <Messages/NotifyPlayerLevel.h>
#include <Messages/NotifyPlayerCellChanged.h>
#include <Messages/NotifyTeleport.h>
#include <Messages/RequestPlayerHealthUpdate.h>
#include <Messages/NotifyPlayerHealthUpdate.h>

#include <Structs/GridCellCoords.h>

#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/ConnectionErrorEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/PartyJoinedEvent.h>
#include <Events/PartyLeftEvent.h>

#include <PlayerCharacter.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/TESObjectCELL.h>
#include <Games/ActorExtension.h>
#include <Games/Skyrim/Interface/UI.h>

#include <xinput.h>

using TiltedPhoques::OverlayRenderHandler;
using TiltedPhoques::OverlayRenderHandlerD3D11;

namespace
{
using TXInputGetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

void InjectControllerKey(OverlayApp* apOverlay, uint16_t aKey, uint32_t aModifiers = 0)
{
    const auto scanCode = static_cast<uint16_t>(MapVirtualKeyW(aKey, MAPVK_VK_TO_VSC));
    apOverlay->InjectKey(KEYEVENT_KEYDOWN, aModifiers, aKey, scanCode);
    apOverlay->InjectKey(KEYEVENT_KEYUP, aModifiers, aKey, scanCode);
}

void PollControllerNavigation(OverlayApp* apOverlay, bool aActive)
{
    static TXInputGetState s_getState = []() -> TXInputGetState {
        for (const wchar_t* library : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
            if (const auto module = LoadLibraryW(library))
                if (const auto proc = GetProcAddress(module, "XInputGetState"))
                    return reinterpret_cast<TXInputGetState>(proc);
        return nullptr;
    }();
    static WORD s_previous = 0;
    static WORD s_repeating = 0;
    static auto s_nextRepeat = std::chrono::steady_clock::time_point{};

    if (!aActive || !apOverlay || !s_getState)
    {
        s_previous = 0;
        s_repeating = 0;
        return;
    }

    XINPUT_STATE state{};
    bool connected = false;
    for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index)
    {
        if (s_getState(index, &state) == ERROR_SUCCESS)
        {
            connected = true;
            break;
        }
    }
    if (!connected)
    {
        s_previous = 0;
        return;
    }

    // Key-binding capture takes the next real button/trigger press instead of
    // menu navigation (raw state: the stick is not folded into the D-pad).
    static WORD s_previousRaw = 0;
    static bool s_previousLeftTrigger = false;
    static bool s_previousRightTrigger = false;
    const WORD rawPressed = state.Gamepad.wButtons & ~s_previousRaw;
    const bool leftTrigger = state.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    const bool rightTrigger = state.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    const bool capturing = ControlBindings::IsCapturing() &&
        ControlBindings::OnGamepadButtons(rawPressed, leftTrigger && !s_previousLeftTrigger,
            rightTrigger && !s_previousRightTrigger);
    s_previousRaw = state.Gamepad.wButtons;
    s_previousLeftTrigger = leftTrigger;
    s_previousRightTrigger = rightTrigger;
    if (capturing || ControlBindings::IsCapturing())
    {
        s_previous = state.Gamepad.wButtons;
        return;
    }

    WORD buttons = state.Gamepad.wButtons;
    if (state.Gamepad.sThumbLY > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) buttons |= XINPUT_GAMEPAD_DPAD_UP;
    if (state.Gamepad.sThumbLY < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (state.Gamepad.sThumbLX < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (state.Gamepad.sThumbLX > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;

    constexpr WORD navigation = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN |
        XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;
    const WORD pressed = buttons & ~s_previous;
    const auto now = std::chrono::steady_clock::now();
    WORD repeated = 0;
    const WORD heldNavigation = buttons & navigation;
    if (heldNavigation != s_repeating)
    {
        s_repeating = heldNavigation;
        s_nextRepeat = now + 400ms;
    }
    else if (heldNavigation && now >= s_nextRepeat)
    {
        repeated = heldNavigation;
        s_nextRepeat = now + 110ms;
    }

    // The overlay's GamepadNavigationService owns what each button means
    // (spatial focus, back, section switching, value changes, prompts), so
    // actions are sent by name rather than as synthetic keys.
    bool any = false;
    const auto send = [&](const char* acAction, bool aRepeat)
    {
        auto pArguments = CefListValue::Create();
        pArguments->SetString(0, acAction);
        pArguments->SetBool(1, aRepeat);
        apOverlay->ExecuteAsync("gamepadInput", pArguments);
        any = true;
    };
    const auto direction = [&](WORD aBit, const char* acAction)
    {
        if (pressed & aBit)
            send(acAction, false);
        else if (repeated & aBit)
            send(acAction, true);
    };
    // One vertical and one horizontal step at most per poll.
    if ((pressed | repeated) & XINPUT_GAMEPAD_DPAD_UP)
        direction(XINPUT_GAMEPAD_DPAD_UP, "up");
    else
        direction(XINPUT_GAMEPAD_DPAD_DOWN, "down");
    if ((pressed | repeated) & XINPUT_GAMEPAD_DPAD_LEFT)
        direction(XINPUT_GAMEPAD_DPAD_LEFT, "left");
    else
        direction(XINPUT_GAMEPAD_DPAD_RIGHT, "right");

    constexpr std::pair<WORD, const char*> cButtons[]{
        {XINPUT_GAMEPAD_A, "a"}, {XINPUT_GAMEPAD_B, "b"}, {XINPUT_GAMEPAD_X, "x"}, {XINPUT_GAMEPAD_Y, "y"},
        {XINPUT_GAMEPAD_LEFT_SHOULDER, "lb"}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, "rb"},
        {XINPUT_GAMEPAD_START, "start"}, {XINPUT_GAMEPAD_BACK, "view"},
        {XINPUT_GAMEPAD_LEFT_THUMB, "ls"}, {XINPUT_GAMEPAD_RIGHT_THUMB, "rs"}};
    for (const auto& [bit, name] : cButtons)
        if (pressed & bit)
            send(name, false);

    static bool s_navLeftTrigger = false;
    static bool s_navRightTrigger = false;
    if (leftTrigger && !s_navLeftTrigger)
        send("lt", false);
    if (rightTrigger && !s_navRightTrigger)
        send("rt", false);
    s_navLeftTrigger = leftTrigger;
    s_navRightTrigger = rightTrigger;

    // Right stick scrolls the focused panel, proportional to deflection.
    static auto s_lastScroll = now;
    const float ry = state.Gamepad.sThumbRY / 32767.f;
    const float deadzone = XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE / 32767.f;
    if (std::abs(ry) > deadzone && now - s_lastScroll >= 16ms)
    {
        const float seconds = std::min(0.1f, std::chrono::duration<float>(now - s_lastScroll).count());
        const float amount = (std::abs(ry) - deadzone) / (1.f - deadzone);
        auto pArguments = CefListValue::Create();
        pArguments->SetDouble(0, -std::copysign(amount * amount * 1400.f * seconds, ry));
        apOverlay->ExecuteAsync("gamepadScroll", pArguments);
        s_lastScroll = now;
        any = true;
    }
    else if (std::abs(ry) <= deadzone)
        s_lastScroll = now;

    if (any)
    {
        InputService::NotifyControllerInput();
        if (const auto client = apOverlay->GetClient())
            if (const auto renderer = client->GetOverlayRenderHandler())
                renderer->SetCursorVisible(false);
    }

    s_previous = buttons;
}
}

struct D3D11RenderProvider final : OverlayApp::RenderProvider, OverlayRenderHandlerD3D11::Renderer
{
    explicit D3D11RenderProvider(RenderSystemD3D11* apRenderSystem)
        : m_pRenderSystem(apRenderSystem)
    {
    }

    OverlayRenderHandler* Create() override
    {
        auto* pHandler = new OverlayRenderHandlerD3D11(this);
        pHandler->SetVisible(true);

        return pHandler;
    }

    [[nodiscard]] HWND GetWindow() override { return m_pRenderSystem->GetWindow(); }

    [[nodiscard]] IDXGISwapChain* GetSwapChain() const noexcept override { return m_pRenderSystem->GetSwapChain(); }
    [[nodiscard]] ID3D11Device* GetDevice() const noexcept override { return m_pRenderSystem->GetDevice(); }
    [[nodiscard]] ID3D11DeviceContext* GetDeviceContext() const noexcept override { return m_pRenderSystem->GetDeviceContext(); }

private:
    RenderSystemD3D11* m_pRenderSystem;
};

String GetCellName(const GameId& aWorldSpaceId, const GameId& aCellId) noexcept
{
    auto& modSystem = World::Get().GetModSystem();

    String cellName = "UNKNOWN";

    if (aWorldSpaceId)
    {
        const uint32_t worldSpaceId = modSystem.GetGameId(aWorldSpaceId);
        TESWorldSpace* pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(worldSpaceId));
        if (pWorldSpace)
            cellName = pWorldSpace->GetName();
    }
    else
    {
        const uint32_t cellId = modSystem.GetGameId(aCellId);
        TESObjectCELL* pCell = Cast<TESObjectCELL>(TESForm::GetById(cellId));
        if (pCell)
            cellName = pCell->GetName();
    }

    return cellName;
}

float CalculateHealthPercentage(Actor* apActor) noexcept
{
    const float maxHealth = apActor->GetActorPermanentValue(ActorValueInfo::kHealth);
    const float tempModHealth = apActor->healthModifiers.temporaryModifier;

    if (maxHealth == 0.f)
        return 0.f;
    const float health = apActor->GetActorValue(ActorValueInfo::kHealth);

    float percentage = health / (maxHealth + tempModHealth) * 100.f;
    if (percentage < 0.f)
        percentage = 0.f;

    return percentage;
}

OverlayService::OverlayService(World& aWorld, TransportService& transport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(transport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&OverlayService::OnUpdate>(this);
    m_connectedConnection = aDispatcher.sink<ConnectedEvent>().connect<&OverlayService::OnConnectedEvent>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&OverlayService::OnDisconnectedEvent>(this);
    m_connectionErrorConnection = aDispatcher.sink<ConnectionErrorEvent>().connect<&OverlayService::OnConnectionError>(this);
    m_chatMessageConnection = aDispatcher.sink<NotifyChatMessageBroadcast>().connect<&OverlayService::OnChatMessageReceived>(this);
    m_playerJoinedConnection = aDispatcher.sink<NotifyPlayerJoined>().connect<&OverlayService::OnPlayerJoined>(this);
    m_playerLeftConnection = aDispatcher.sink<NotifyPlayerLeft>().connect<&OverlayService::OnPlayerLeft>(this);
    m_playerDialogueConnection = aDispatcher.sink<NotifyPlayerDialogue>().connect<&OverlayService::OnPlayerDialogue>(this);
    m_playerAddedConnection = m_world.on_destroy<WaitingFor3D>().connect<&OverlayService::OnWaitingFor3DRemoved>(this);
    m_playerRemovedConnection = m_world.on_destroy<PlayerComponent>().connect<&OverlayService::OnPlayerComponentRemoved>(this);
    m_playerLevelConnection = aDispatcher.sink<NotifyPlayerLevel>().connect<&OverlayService::OnPlayerLevel>(this);
    m_cellChangedConnection = aDispatcher.sink<NotifyPlayerCellChanged>().connect<&OverlayService::OnPlayerCellChanged>(this);
    m_teleportConnection = aDispatcher.sink<NotifyTeleport>().connect<&OverlayService::OnNotifyTeleport>(this);
    m_playerHealthConnection = aDispatcher.sink<NotifyPlayerHealthUpdate>().connect<&OverlayService::OnNotifyPlayerHealthUpdate>(this);
    m_partyJoinedConnection = aDispatcher.sink<PartyJoinedEvent>().connect<&OverlayService::OnPartyJoinedEvent>(this);
    m_partyLeftConnection = aDispatcher.sink<PartyLeftEvent>().connect<&OverlayService::OnPartyLeftEvent>(this);
}

OverlayService::~OverlayService() noexcept
{
}

void OverlayService::Create(RenderSystemD3D11* apRenderSystem) noexcept
{
    m_pProvider = TiltedPhoques::MakeUnique<D3D11RenderProvider>(apRenderSystem);
    m_pOverlay = new OverlayApp(m_pProvider.get(), new ::OverlayClient(m_transport, m_pProvider->Create()));

    if (!m_pOverlay->Initialize())
    {
        spdlog::error("Overlay could not be initialized");
        if (int32_t exitCode = CefGetExitCode())
        {
            spdlog::critical("CEF failed to initialize, exit code {}. See 'cef_types.h' for description", exitCode);
        }
    }

    m_pOverlay->GetClient()->Create();
}

void OverlayService::Render() noexcept
{
    static bool s_f9WasDown = false;
    static bool s_f10WasDown = false;
    static bool s_debugPromptOpen = false;
    static bool s_titleBaselineScheduled = false;
    static std::chrono::steady_clock::time_point s_mainMenuDumpAt{};

    PollControllerNavigation(m_pOverlay.get(), m_active);

    const auto dumpMainMenuState = [this]() {
        uint32_t overlayWidth = 0;
        uint32_t overlayHeight = 0;
        if (m_pOverlay && m_pOverlay->GetClient())
        {
            if (auto pRenderer = m_pOverlay->GetClient()->GetOverlayRenderHandler())
                std::tie(overlayWidth, overlayHeight) = pRenderer->GetRenderSize();
        }
        DumpMainMenuState(overlayWidth, overlayHeight);
    };

    auto* pUI = UI::Get();
    const bool titleScreen = pUI && pUI->GetMenuOpen(BSFixedString("Main Menu"));
    if (titleScreen)
        PollMainMenuOptions(pUI->FindMenuByName(BSFixedString("Main Menu")));
    SetMainMenuOverlayActive(m_active && titleScreen);
    if (titleScreen && !s_titleBaselineScheduled)
    {
        // Wait until Skyrim, Scaleform, Mist Menu, and the CEF render target have
        // all advanced beyond their first initialization frame.
        s_mainMenuDumpAt = std::chrono::steady_clock::now() + 1500ms;
        s_titleBaselineScheduled = true;
    }
    else if (!titleScreen)
    {
        s_titleBaselineScheduled = false;
        s_mainMenuDumpAt = {};
    }

    const bool f11Down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (f11Down && !m_f11WasDown)
    {
        m_world.GetGameSettingsService().ToggleWindowMode();
        // Window mode switching is asynchronous. Capture the stable result,
        // not the frame containing the resize request.
        if (titleScreen)
            s_mainMenuDumpAt = std::chrono::steady_clock::now() + 2000ms;
    }
    m_f11WasDown = f11Down;

    const bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9Down && !s_f9WasDown)
        dumpMainMenuState();
    s_f9WasDown = f9Down;

    if (titleScreen && s_mainMenuDumpAt != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() >= s_mainMenuDumpAt)
    {
        dumpMainMenuState();
        s_mainMenuDumpAt = {};
    }

    const bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10Down && !s_f10WasDown)
    {
        if (s_debugPromptOpen && m_pOverlay)
        {
            spdlog::info("F10 in-game problem report submitted without mouse input");
            m_pOverlay->ExecuteAsync("submitDebugPrompt");
            s_debugPromptOpen = false;
        }
        else
        {
            spdlog::info("F10 in-game problem report requested");
            ShowDebugPrompt("Describe what is wrong, then press F10 again to capture and send. Press Escape to cancel.", true);
            s_debugPromptOpen = true;
        }
    }
    s_f10WasDown = f10Down;

    const bool escapeDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    if (escapeDown && !m_escapeWasDown && m_active)
    {
        if (s_debugPromptOpen && m_pOverlay)
        {
            m_pOverlay->ExecuteAsync("cancelDebugPrompt");
            s_debugPromptOpen = false;
            spdlog::info("In-game problem report cancelled with Escape");
        }
        m_world.GetGameSettingsService().RevertSettings();
        SetActive(false);
    }
    m_escapeWasDown = escapeDown;

    if (!m_active)
        s_debugPromptOpen = false;

    if (titleScreen != m_titleScreen)
    {
        m_titleScreen = titleScreen;
        SetVersion(BUILD_COMMIT);
        m_pOverlay->ExecuteAsync(m_titleScreen ? "enterTitleScreen" : "exitTitleScreen");
        if (!m_titleScreen && !m_inGame)
            SetActive(false);
    }

    auto pPlayer = PlayerCharacter::Get();
    bool inGame = pPlayer && pPlayer->GetNiNode();
    if (inGame && !m_inGame)
        SetInGame(true);
    else if (!inGame && m_inGame)
        SetInGame(false);

    m_pOverlay->GetClient()->Render();
    if (m_active)
        RenderNativeCursorOnTop();
}

void OverlayService::Reset() const noexcept
{
    m_pOverlay->GetClient()->Reset();
}

void OverlayService::Reload() noexcept
{
    SetInGame(false);
    SetActive(false);
    GetOverlayApp()->GetClient()->GetBrowser()->Reload();
    Initialize();
    SetInGame(true);
    m_pOverlay->ExecuteAsync("enterGame");
    SetActive(true);
}

void OverlayService::Initialize() noexcept
{
    m_pOverlay->ExecuteAsync("init");
}

void OverlayService::SetActive(bool aActive) noexcept
{
    if (!m_inGame && !m_titleScreen)
        return;
    if (m_active == aActive)
        return;

    m_active = aActive;
    SetMainMenuOverlayActive(m_active && m_titleScreen);

    TiltedPhoques::DInputHook::Get().SetEnabled(m_active);
    if (m_pOverlay && m_pOverlay->GetClient())
    {
        if (auto pRenderer = m_pOverlay->GetClient()->GetOverlayRenderHandler())
            pRenderer->SetCursorVisible(false);
    }

    // Pointer visibility/confinement is owned by InputService on the window thread.
    InputService::RequestCursorUpdate();

    m_pOverlay->ExecuteAsync(m_active ? "activate" : "deactivate");
}

bool OverlayService::GetActive() const noexcept
{
    return m_active;
}

void OverlayService::SetInGame(bool aInGame) noexcept
{
    if (m_inGame == aInGame)
        return;
    m_inGame = aInGame;

    if (m_inGame)
    {
        SetVersion(BUILD_COMMIT);
        m_pOverlay->ExecuteAsync("enterGame");
    }
    else
    {
        m_pOverlay->ExecuteAsync("exitGame");
        // TODO: this does nothing, since m_inGame is false
        SetActive(false);
    }
}

bool OverlayService::GetInGame() const noexcept
{
    return m_inGame;
}

void OverlayService::SetVersion(const std::string& acVersion)
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();

    pArguments->SetString(0, acVersion);
    m_pOverlay->ExecuteAsync("setVersion", pArguments);
}

void OverlayService::SendSystemMessage(const std::string& acMessage)
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, kSystemMessage);
    pArguments->SetString(1, acMessage);

    m_pOverlay->ExecuteAsync("message", pArguments);
}

void OverlayService::ShowDebugPrompt(const std::string& acMessage, bool aNoteOnly)
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetString(0, acMessage);
    pArguments->SetBool(1, aNoteOnly);
    spdlog::info("Dispatching debugPrompt event to overlay");
    m_pOverlay->ExecuteAsync("debugPrompt", pArguments);
    SetActive(true);
}

bool OverlayService::InjectTestControllerButton(const std::string& acButton) noexcept
{
    if (!m_pOverlay || !m_active)
        return false;

    uint16_t key = 0;
    if (acButton == "up") key = VK_UP;
    else if (acButton == "down") key = VK_DOWN;
    else if (acButton == "left") key = VK_LEFT;
    else if (acButton == "right") key = VK_RIGHT;
    else if (acButton == "a") key = VK_SPACE;
    else if (acButton == "b") key = VK_ESCAPE;
    else return false;

    InjectControllerKey(m_pOverlay.get(), key);
    InputService::NotifyControllerInput();
    if (const auto client = m_pOverlay->GetClient())
        if (const auto renderer = client->GetOverlayRenderHandler())
            renderer->SetCursorVisible(false);
    return true;
}

void OverlayService::SetPlayerHealthPercentage(uint32_t aFormId) const noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
    if (!pActor)
    {
        spdlog::error("{}: cannot find actor for form id {:X}", __FUNCTION__, aFormId);
        return;
    }

    float percentage = CalculateHealthPercentage(pActor);

    auto view = m_world.view<FormIdComponent, PlayerComponent>();
    auto entityIt = std::find_if(view.begin(), view.end(), [view, aFormId](auto aEntity) { return view.get<FormIdComponent>(aEntity).Id == aFormId; });

    if (entityIt == view.end())
    {
        spdlog::error("{}: cannot find player entity for form id {:X}", __FUNCTION__, aFormId);
        return;
    }

    const auto& playerComponent = view.get<PlayerComponent>(*entityIt);

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, playerComponent.Id);
    pArguments->SetDouble(1, static_cast<double>(percentage));
    m_pOverlay->ExecuteAsync("setHealth", pArguments);
}

void OverlayService::OnUpdate(const UpdateEvent&) noexcept
{
    RunDebugDataUpdates();
    RunPlayerHealthUpdates();
}

void OverlayService::OnConnectedEvent(const ConnectedEvent& acEvent) noexcept
{
    m_pOverlay->ExecuteAsync("connect");

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acEvent.PlayerId);
    m_pOverlay->ExecuteAsync("setLocalPlayerId", pArguments);
}

void OverlayService::OnDisconnectedEvent(const DisconnectedEvent&) noexcept
{
    m_pOverlay->ExecuteAsync("disconnect");
}

void OverlayService::OnWaitingFor3DRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept
{
    const auto* pPlayerComponent = m_world.try_get<PlayerComponent>(aEntity);
    if (!pPlayerComponent)
        return;

    const auto& formIdComponent = m_world.get<FormIdComponent>(aEntity);

    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
    if (!pActor)
    {
        spdlog::error("{}: cannot find actor for form id {:X}", __FUNCTION__, formIdComponent.Id);
        return;
    }

    float percentage = CalculateHealthPercentage(pActor);

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, pPlayerComponent->Id);
    pArguments->SetInt(1, static_cast<int>(percentage));

    m_pOverlay->ExecuteAsync("setPlayer3dLoaded", pArguments);
}

void OverlayService::OnPlayerComponentRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept
{
    const auto& playerComponent = m_world.get<PlayerComponent>(aEntity);

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, playerComponent.Id);

    m_pOverlay->ExecuteAsync("setPlayer3dUnloaded", pArguments);
}

void OverlayService::OnChatMessageReceived(const NotifyChatMessageBroadcast& acMessage) noexcept
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, (int)acMessage.MessageType);
    pArguments->SetString(1, acMessage.ChatMessage.c_str());
    pArguments->SetString(2, acMessage.PlayerName.c_str());

    m_pOverlay->ExecuteAsync("message", pArguments);
}

void OverlayService::OnPlayerDialogue(const NotifyPlayerDialogue& acMessage) noexcept
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, kPlayerDialogue);
    pArguments->SetString(1, acMessage.Text.c_str());
    pArguments->SetString(2, acMessage.Name.c_str());

    m_pOverlay->ExecuteAsync("message", pArguments);
}

void OverlayService::OnConnectionError(const ConnectionErrorEvent& acConnectedEvent) const noexcept
{
    auto pArgs = CefListValue::Create();
    pArgs->SetString(0, acConnectedEvent.ErrorDetail.c_str());
    m_pOverlay->ExecuteAsync("triggerError", pArgs);
}

void OverlayService::OnPlayerJoined(const NotifyPlayerJoined& acMessage) noexcept
{
    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    pArguments->SetString(1, acMessage.Username.c_str());
    pArguments->SetInt(2, acMessage.Level);

    String cellName = GetCellName(acMessage.WorldSpaceId, acMessage.CellId);
    pArguments->SetString(3, cellName.c_str());

    m_pOverlay->ExecuteAsync("playerConnected", pArguments);
}

void OverlayService::OnPlayerLeft(const NotifyPlayerLeft& acMessage) noexcept
{
    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    pArguments->SetString(1, acMessage.Username.c_str());
    m_pOverlay->ExecuteAsync("playerDisconnected", pArguments);
}

void OverlayService::OnPlayerLevel(const NotifyPlayerLevel& acMessage) noexcept
{
    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    pArguments->SetInt(1, acMessage.NewLevel);
    m_pOverlay->ExecuteAsync("setLevel", pArguments);
}

void OverlayService::OnPlayerCellChanged(const NotifyPlayerCellChanged& acMessage) const noexcept
{
    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    String cellName = GetCellName(acMessage.WorldSpaceId, acMessage.CellId);
    pArguments->SetString(1, cellName.c_str());
    m_pOverlay->ExecuteAsync("setCell", pArguments);
}

void OverlayService::OnNotifyTeleport(const NotifyTeleport& acMessage) noexcept
{
    auto& modSystem = m_world.GetModSystem();

    const uint32_t cellId = modSystem.GetGameId(acMessage.CellId);
    TESObjectCELL* pCell = Cast<TESObjectCELL>(TESForm::GetById(cellId));
    if (!pCell)
    {
        const uint32_t worldSpaceId = modSystem.GetGameId(acMessage.WorldSpaceId);
        TESWorldSpace* pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(worldSpaceId));
        if (pWorldSpace)
        {
            GridCellCoords coordinates = GridCellCoords::CalculateGridCellCoords(acMessage.Position);
            pCell = pWorldSpace->LoadCell(coordinates.X, coordinates.Y);
        }

        if (!pCell)
        {
            spdlog::error("Failed to fetch cell to teleport to.");
            m_world.GetOverlayService().SendSystemMessage("Teleporting to player failed.");
            return;
        }
    }

    PlayerCharacter::Get()->MoveTo(pCell, acMessage.Position);
}

void OverlayService::OnNotifyPlayerHealthUpdate(const NotifyPlayerHealthUpdate& acMessage) noexcept
{
    const float percentage = acMessage.Percentage >= 0.f ? acMessage.Percentage : 0.f;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    pArguments->SetDouble(1, static_cast<double>(percentage));
    m_pOverlay->ExecuteAsync("setHealth", pArguments);
}

void OverlayService::OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept
{
    if (acEvent.IsLeader)
        m_world.GetOverlayService().GetOverlayApp()->ExecuteAsync("partyCreated");
}

void OverlayService::OnPartyLeftEvent(const PartyLeftEvent& acEvent) noexcept
{
    m_world.GetOverlayService().GetOverlayApp()->ExecuteAsync("partyLeft");
}

void OverlayService::RunDebugDataUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 1000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    auto internalStats = m_transport.GetStatistics();
    auto steamStats = m_transport.GetConnectionStatus();

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, steamStats.m_flOutPacketsPerSec);
    pArguments->SetInt(1, steamStats.m_flInPacketsPerSec);
    pArguments->SetInt(2, steamStats.m_nPing);
    pArguments->SetInt(3, 0);
    pArguments->SetInt(4, internalStats.SentBytes);
    pArguments->SetInt(5, internalStats.RecvBytes);

    m_pOverlay->ExecuteAsync("debugData", pArguments);
}

// TODO(cosideci): this whole thing is a really hacky solution to
// health sync code being somewhat broken for players.
void OverlayService::RunPlayerHealthUpdates() noexcept
{
    if (!m_transport.IsConnected() || !m_world.GetPartyService().IsInParty())
        return;

    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 500ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    static float s_previousPercentage = -1.f;

    const float newPercentage = CalculateHealthPercentage(PlayerCharacter::Get());
    if (newPercentage == s_previousPercentage)
        return;

    s_previousPercentage = newPercentage;

    RequestPlayerHealthUpdate request{};
    request.Percentage = newPercentage;

    m_transport.Send(request);
}
