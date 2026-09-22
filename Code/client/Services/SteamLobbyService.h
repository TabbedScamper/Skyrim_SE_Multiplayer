#pragma once

#include <Events/UpdateEvent.h>
#include <Events/ConnectedEvent.h>

struct World;

struct SteamLobbyService
{
    SteamLobbyService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~SteamLobbyService() noexcept;

    TP_NOCOPYMOVE(SteamLobbyService);

    void HostSession() noexcept;
    void JoinSession(const String& acLobbyId) noexcept;
    void JoinFriend(uint64_t aSteamId) noexcept;
    void InviteFriend() noexcept;
    void RefreshLobbyState() noexcept;
    void ApplyPartySettings(bool aOpen, bool aPasswordProtected) noexcept;
    void ConnectJoinedSession(const String& acPassword) noexcept;
    void LeaveSession() noexcept;
    void PumpCallbacks() noexcept;

private:
    enum class PendingOperation
    {
        None,
        Create,
        Join
    };

    void OnUpdate(const UpdateEvent&) noexcept;
    void OnConnected(const ConnectedEvent&) noexcept;
    bool Initialize() noexcept;
    bool StartLocalServer() noexcept;
    void StopLocalServer() noexcept;
    String GetLanEndpoint() const noexcept;
    void CompleteCreate() noexcept;
    void CompleteJoin() noexcept;
    void ShowMessage(const String& acMessage) const noexcept;
    void PublishLobbyState() noexcept;

    World& m_world;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_connectedConnection;
    HMODULE m_steamModule{};
    void* m_matchmaking{};
    void* m_utils{};
    void* m_friends{};
    uint64_t m_apiCall{};
    uint64_t m_lobbyId{};
    bool m_isHost{};
    bool m_lobbyOpen{};
    bool m_passwordProtected{};
    bool m_waitingForPassword{};
    bool m_autoHostAttempted{};
    String m_joinEndpoint{};
    uint64_t m_nextLobbyRefresh{};
    PendingOperation m_pending{PendingOperation::None};
    HANDLE m_serverProcess{};
    HANDLE m_serverThread{};
    HANDLE m_serverJob{};
};
