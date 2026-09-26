#pragma once

#include <include/internal/cef_ptr.h>

namespace TiltedPhoques
{
struct OverlayApp;
}

struct RenderSystemD3D11;
struct D3D11RenderProvider;
struct FormIdComponent;
struct World;
struct Actor;
struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct TransportService;
struct NotifyChatMessageBroadcast;
struct NotifyPlayerList;
struct NotifyPlayerJoined;
struct NotifyPlayerLeft;
struct NotifyPlayerDialogue;
struct ConnectionErrorEvent;
struct NotifyPlayerLevel;
struct NotifyPlayerCellChanged;
struct NotifyTeleport;
struct NotifyPlayerHealthUpdate;
enum ChatMessageTypes;
struct PartyJoinedEvent;
struct PartyLeftEvent;
struct DialogueListenState;

using TiltedPhoques::OverlayApp;

/**
 * @brief Controls the UI from the client.
 */
struct OverlayService
{
    OverlayService(World& aWorld, TransportService& transport, entt::dispatcher& aDispatcher);
    ~OverlayService() noexcept;

    TP_NOCOPYMOVE(OverlayService);

    void Create(RenderSystemD3D11* apRenderSystem) noexcept;

    void Render() noexcept;
    void Reset() const noexcept;
    void Reload() noexcept;

    void Initialize() noexcept;
    void PushDialogueListen(const DialogueListenState* aState, uint32_t aSpeaker);

    void SetActive(bool aActive) noexcept;
    [[nodiscard]] bool GetActive() const noexcept;

    void SetInGame(bool aInGame) noexcept;
    [[nodiscard]] bool GetInGame() const noexcept;
    [[nodiscard]] bool GetTitleScreen() const noexcept { return m_titleScreen; }

    void SetVersion(const std::string& acVersion);

    OverlayApp* GetOverlayApp() const noexcept { return m_pOverlay.get(); }

    void SendSystemMessage(const std::string& acMessage);
    // Character creation together: the banner over the creator (whose character is shown, ready or not).
    void SetCreatorView(bool aVisible, int aPlayer, int aPlayerCount, bool aReady, bool aLocalReady = false, bool aGamepad = false);
    void ShowDebugPrompt(const std::string& acMessage, bool aNoteOnly = false);
    bool InjectTestControllerButton(const std::string& acButton) noexcept;

    void SetPlayerHealthPercentage(uint32_t aFormId) const noexcept;

protected:
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnConnectedEvent(const ConnectedEvent&) noexcept;
    void OnDisconnectedEvent(const DisconnectedEvent&) noexcept;
    void OnWaitingFor3DRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept;
    void OnPlayerComponentRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept;
    void OnConnectionError(const ConnectionErrorEvent& acConnectedEvent) const noexcept;
    void OnChatMessageReceived(const NotifyChatMessageBroadcast&) noexcept;
    void OnPlayerDialogue(const NotifyPlayerDialogue&) noexcept;
    void OnPlayerJoined(const NotifyPlayerJoined&) noexcept;
    void OnPlayerLeft(const NotifyPlayerLeft&) noexcept;
    void OnPlayerLevel(const NotifyPlayerLevel&) noexcept;
    void OnPlayerCellChanged(const NotifyPlayerCellChanged& acMessage) const noexcept;
    void OnNotifyTeleport(const NotifyTeleport& acMessage) noexcept;
    void OnNotifyPlayerHealthUpdate(const NotifyPlayerHealthUpdate& acMessage) noexcept;
    void OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept;
    void OnPartyLeftEvent(const PartyLeftEvent& acEvent) noexcept;

private:
    void RunDebugDataUpdates() noexcept;
    void RunPlayerHealthUpdates() noexcept;

    CefRefPtr<OverlayApp> m_pOverlay{nullptr};
    TiltedPhoques::UniquePtr<D3D11RenderProvider> m_pProvider;

    World& m_world;
    TransportService& m_transport;

    bool m_active = false;
    bool m_inGame = false;
    bool m_titleScreen = false;
    bool m_f11WasDown = false;
    bool m_escapeWasDown = false;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_connectionErrorConnection;
    entt::scoped_connection m_cellChangeEventConnection;
    entt::scoped_connection m_chatMessageConnection;
    entt::scoped_connection m_playerDialogueConnection;
    entt::scoped_connection m_playerJoinedConnection;
    entt::scoped_connection m_playerLeftConnection;
    entt::scoped_connection m_playerAddedConnection;
    entt::scoped_connection m_playerRemovedConnection;
    entt::scoped_connection m_playerLevelConnection;
    entt::scoped_connection m_cellChangedConnection;
    entt::scoped_connection m_teleportConnection;
    entt::scoped_connection m_playerHealthConnection;
    entt::scoped_connection m_partyJoinedConnection;
    entt::scoped_connection m_partyLeftConnection;
};
