#include <TiltedOnlinePCH.h>

#include <OverlayRenderHandler.hpp>
#include <DInputHook.hpp>

#include <Services/OverlayClient.h>
#include <Services/TransportService.h>

#include <Messages/SendChatMessageRequest.h>
#include <Messages/TeleportRequest.h>

#include <Events/SetTimeCommandEvent.h>

#include <World.h>

OverlayClient::OverlayClient(TransportService& aTransport, TiltedPhoques::OverlayRenderHandler* apHandler)
    : TiltedPhoques::OverlayClient(apHandler)
    , m_transport(aTransport)
{
}

OverlayClient::~OverlayClient() noexcept
{
}

bool OverlayClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process, CefRefPtr<CefProcessMessage> message)
{
    if (message->GetName() == "ui-event")
    {
        auto pArguments = message->GetArgumentList();

        auto eventName = pArguments->GetString(0).ToString();
        auto eventArgs = pArguments->GetList(1);

        // UI arguments can contain lobby and server passwords. Never log them.
        spdlog::debug("Received UI event {}", eventName);

#ifndef PUBLIC_BUILD
        LOG(INFO) << "event=ui_event name=" << eventName;
#endif

        if (eventName == "connect")
            ProcessConnectMessage(eventArgs);
        else if (eventName == "disconnect")
            ProcessDisconnectMessage();
        else if (eventName == "revealPlayers")
            ProcessRevealPlayersMessage();
        else if (eventName == "sendMessage")
            ProcessChatMessage(eventArgs);
        else if (eventName == "setTime")
            ProcessSetTimeCommand(eventArgs);
        else if (eventName == "launchParty")
            World::Get().GetPartyService().CreateParty();
        else if (eventName == "leaveParty")
            World::Get().GetPartyService().LeaveParty();
        else if (eventName == "createPartyInvite")
        {
            uint32_t aPlayerId = eventArgs->GetInt(0);
            World::Get().GetPartyService().CreateInvite(aPlayerId);
        }
        else if (eventName == "acceptPartyInvite")
        {
            uint32_t aInviterId = eventArgs->GetInt(0);
            // push to main thread because the party service has to check validity of invite thread safely
            World::Get().GetRunner().Queue([aInviterId]() { World::Get().GetPartyService().AcceptInvite(aInviterId); });
        }
        else if (eventName == "kickPartyMember")
        {
            uint32_t aPlayerId = eventArgs->GetInt(0);
            World::Get().GetPartyService().KickPartyMember(aPlayerId);
        }
        else if (eventName == "changePartyLeader")
        {
            uint32_t aPlayerId = eventArgs->GetInt(0);
            World::Get().GetPartyService().ChangePartyLeader(aPlayerId);
        }
        else if (eventName == "setPartyReady")
            World::Get().GetPartyService().SetReady(eventArgs->GetBool(0));
        else if (eventName == "selectSharedCampaign")
        {
            const auto mode = static_cast<uint8_t>(eventArgs->GetInt(0));
            const String checkpoint = eventArgs->GetString(1).ToString().c_str();
            World::Get().GetPartyService().SelectCampaign(mode, checkpoint);
        }
        else if (eventName == "startTogether")
        {
            const auto mode = static_cast<uint8_t>(eventArgs->GetInt(0));
            const String checkpoint = eventArgs->GetString(1).ToString().c_str();
            World::Get().GetPartyService().StartTogether(mode, checkpoint);
        }
        else if (eventName == "teleportToPlayer")
            ProcessTeleportMessage(eventArgs);
        else if (eventName == "toggleDebugUI")
            ProcessToggleDebugUI();
        else if (eventName == "hostSteamSession")
            World::Get().GetSteamLobbyService().QueueHostSession();
        else if (eventName == "joinSteamSession")
        {
            const auto lobbyId = eventArgs->GetString(0).ToString();
            World::Get().GetSteamLobbyService().QueueJoinSession(lobbyId.c_str());
        }
        else if (eventName == "leaveSteamSession")
            World::Get().GetSteamLobbyService().QueueLeaveSession();
        else if (eventName == "joinSteamFriend")
        {
            try
            {
                const auto steamId = std::stoull(eventArgs->GetString(0).ToString());
                World::Get().GetSteamLobbyService().QueueJoinFriend(steamId);
            }
            catch (...)
            {
                spdlog::warn("Rejected invalid Steam friend id from UI");
            }
        }
        else if (eventName == "inviteSteamFriend")
            World::Get().GetSteamLobbyService().QueueInviteFriend();
        else if (eventName == "refreshSteamLobby")
            World::Get().GetSteamLobbyService().QueueRefreshLobbyState();
        else if (eventName == "setSteamSessionAccess")
        {
            const bool open = eventArgs->GetBool(0);
            const String password = eventArgs->GetString(1).ToString().c_str();
            World::Get().GetPartyService().SetSessionSettings(open, password);
        }
        else if (eventName == "setCoopGameplaySettings")
        {
            const int difficulty = eventArgs->GetInt(0);
            const bool pvp = eventArgs->GetBool(1);
            if (difficulty >= 0 && difficulty <= 5)
                World::Get().GetRunner().Queue([difficulty, pvp]() {
                    World::Get().GetPartyService().SetGameplaySettings(
                        static_cast<uint32_t>(difficulty), pvp);
                });
        }
        else if (eventName == "connectJoinedSteamSession")
        {
            const String password = eventArgs->GetString(0).ToString().c_str();
            World::Get().GetSteamLobbyService().QueueConnectJoinedSession(password);
        }
        else if (eventName == "requestGameSettings")
            World::Get().GetGameSettingsService().QueueRequestSettings();
        else if (eventName == "previewGameSetting")
        {
            const String name = eventArgs->GetString(0).ToString().c_str();
            const String value = eventArgs->GetString(1).ToString().c_str();
            World::Get().GetGameSettingsService().QueuePreviewSetting(name, value);
        }
        else if (eventName == "requestControlBindings")
            World::Get().GetGameSettingsService().QueueRequestControlBindings();
        else if (eventName == "startControlCapture")
        {
            const String event = eventArgs->GetString(0).ToString().c_str();
            World::Get().GetGameSettingsService().QueueStartControlCapture(event, eventArgs->GetInt(1));
        }
        else if (eventName == "audioPreviewKeepAlive")
            World::Get().GetGameSettingsService().QueueAudioPreviewKeepAlive(eventArgs->GetString(0).ToString().c_str());
        else if (eventName == "audioPreviewStop")
            World::Get().GetGameSettingsService().QueueAudioPreviewStop();
        else if (eventName == "cancelControlCapture")
            World::Get().GetGameSettingsService().QueueCancelControlCapture();
        else if (eventName == "confirmDisplaySettings")
            World::Get().GetGameSettingsService().QueueConfirmDisplaySettings();
        else if (eventName == "applyGameSettings")
        {
            GameSettingsSnapshot settings{};
            settings.DisplayMode = eventArgs->GetInt(0);
            settings.Monitor = eventArgs->GetInt(1);
            settings.Width = eventArgs->GetInt(2);
            settings.Height = eventArgs->GetInt(3);
            settings.VSync = eventArgs->GetBool(4);
            settings.MasterVolume = eventArgs->GetDouble(5);
            settings.FootstepsVolume = eventArgs->GetDouble(6);
            settings.VoiceVolume = eventArgs->GetDouble(7);
            settings.MusicVolume = eventArgs->GetDouble(8);
            settings.EffectsVolume = eventArgs->GetDouble(9);
            settings.Gamma = eventArgs->GetDouble(10);
            settings.MouseSensitivity = eventArgs->GetDouble(11);
            settings.GamepadSensitivity = eventArgs->GetDouble(12);
            settings.InvertY = eventArgs->GetBool(13);
            settings.DialogueSubtitles = eventArgs->GetBool(14);
            settings.GeneralSubtitles = eventArgs->GetBool(15);
            settings.AlwaysRun = eventArgs->GetBool(16);
            settings.ControllerRumble = eventArgs->GetBool(17);
            World::Get().GetGameSettingsService().QueueApplySettings(settings);
        }
        else if (eventName == "revertGameSettings")
            World::Get().GetGameSettingsService().QueueRevertSettings();
        else if (eventName == "resetGameSettings")
            World::Get().GetGameSettingsService().QueueResetSettings(
                eventArgs->GetSize() > 0 ? eventArgs->GetString(0).ToString() : std::string{});
        else if (eventName == "submitDebugFeedback")
        {
            const bool looksRight = eventArgs->GetBool(0);
            const String note = eventArgs->GetString(1).ToString().c_str();
            // This is a diagnostic Win32/GDI capture and is safe to perform on
            // the CEF browser callback. Queuing it on the game runner can leave
            // reports permanently pending while Skyrim is on the main menu.
            spdlog::info("Processing in-game problem report immediately");
            World::Get().GetGameSettingsService().RecordDebugFeedback(looksRight, note);
        }
        else if (eventName == "openTitleOptions")
            SetUIVisible(true);
        else if (eventName == "openTitleLobby")
            SetUIVisible(true);
        else if (eventName == "deactivate")
            SetUIVisible(false);

        return true;
    }

    return false;
}

void OverlayClient::ProcessConnectMessage(CefRefPtr<CefListValue> aEventArgs)
{
    std::string baseIp = aEventArgs->GetString(0);
    if (baseIp == "localhost")
    {
        baseIp = "127.0.0.1";
    }

    uint16_t port = aEventArgs->GetInt(1) ? aEventArgs->GetInt(1) : 10578;
    World::Get().GetTransport().SetServerPassword(aEventArgs->GetString(2));

    std::string endpoint = baseIp + ":" + std::to_string(port);

    World::Get().GetRunner().Queue([endpoint] { World::Get().GetTransport().Connect(endpoint); });
}

void OverlayClient::ProcessDisconnectMessage()
{
    World::Get().GetRunner().Queue([]() { World::Get().GetTransport().Close(); });
}

void OverlayClient::ProcessRevealPlayersMessage()
{
    SetUIVisible(false);
    World::Get().GetMagicService().StartRevealingOtherPlayers();
}

void OverlayClient::ProcessChatMessage(CefRefPtr<CefListValue> aEventArgs)
{
    std::string contents = aEventArgs->GetString(1).ToString();
    if (!contents.empty())
    {
        SendChatMessageRequest messageRequest;
        messageRequest.MessageType = static_cast<ChatMessageType>(aEventArgs->GetInt(0));
        messageRequest.ChatMessage = contents;

        spdlog::info(L"Send chat message of type {}: '{}' ", messageRequest.MessageType, aEventArgs->GetString(1).ToWString());

        m_transport.Send(messageRequest);
    }
}

void OverlayClient::ProcessSetTimeCommand(CefRefPtr<CefListValue> aEventArgs)
{
    const uint8_t hours = static_cast<uint8_t>(aEventArgs->GetInt(0));
    const uint8_t minutes = static_cast<uint8_t>(aEventArgs->GetInt(1));
    const uint32_t senderId = m_transport.GetLocalPlayerId();
    World::Get().GetDispatcher().trigger(SetTimeCommandEvent(hours, minutes, senderId));
}

void OverlayClient::ProcessTeleportMessage(CefRefPtr<CefListValue> aEventArgs)
{
    TeleportRequest request{};
    request.PlayerId = aEventArgs->GetInt(0);

    m_transport.Send(request);
}

void OverlayClient::ProcessToggleDebugUI()
{
    World::Get().GetDebugService().m_showDebugStuff = !World::Get().GetDebugService().m_showDebugStuff;
}

void OverlayClient::SetUIVisible(bool aVisible) noexcept
{
    auto pRenderer = GetOverlayRenderHandler();
    if (!pRenderer)
        return;

    TiltedPhoques::DInputHook::Get().SetEnabled(aVisible);
    World::Get().GetOverlayService().SetActive(aVisible);
    pRenderer->SetCursorVisible(false);
}
