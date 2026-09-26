#pragma once

#include <TiltedCore/TaskQueue.hpp>

#include <Events/UpdateEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>

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
    // Invites one friend to this lobby directly (no Steam overlay needed).
    void InviteFriendDirect(uint64_t aSteamId) noexcept;
    // Accepts (joins) or dismisses an invite shown in the co-op menu.
    void AnswerInvite(uint64_t aLobbyId, bool aAccept) noexcept;
    void RefreshLobbyState() noexcept;
    void ApplyPartySettings(bool aOpen, bool aPasswordProtected) noexcept;
    void ConnectJoinedSession(const String& acPassword) noexcept;
    void LeaveSession() noexcept;
    void PumpCallbacks() noexcept;

    // CEF title-screen events arrive outside the gameplay update loop. Queue
    // them here so PollMainMenuOptions can execute them on Skyrim's main
    // thread while it pumps Steam callbacks.
    void QueueHostSession() noexcept;
    void QueueJoinSession(String aLobbyId) noexcept;
    void QueueLeaveSession() noexcept;
    void QueueJoinFriend(uint64_t aSteamId) noexcept;
    void QueueInviteFriend() noexcept;
    void QueueInviteFriendDirect(uint64_t aSteamId) noexcept;
    void QueueAnswerInvite(uint64_t aLobbyId, bool aAccept) noexcept;
    void QueueRefreshLobbyState() noexcept;
    void QueueConnectJoinedSession(String aPassword) noexcept;

    // Test bridge: the last state published to the UI, as JSON (any thread).
    std::string TestStateJson() const noexcept;

    // Adapter registered with SteamAPI_RegisterCallback (defined in the .cpp).
    struct CallbackBridge;

private:
    enum class PendingOperation
    {
        None,
        Create,
        Join
    };

    void OnUpdate(const UpdateEvent&) noexcept;
    void OnConnected(const ConnectedEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    bool Initialize() noexcept;
    bool StartLocalServer() noexcept;
    void StopLocalServer() noexcept;
    String GetLanEndpoint() const noexcept;
    void CompleteCreate() noexcept;
    void CompleteJoin() noexcept;
    void ShowMessage(const String& acMessage) const noexcept;
    void PublishLobbyState() noexcept;
    void RegisterSteamCallbacks() noexcept;
    void UpdateRichPresence() noexcept;
    void SendAvatar(uint64_t aSteamId) noexcept;
    // Steam callbacks (dispatched by SteamAPI_RunCallbacks from PumpCallbacks).
    void OnJoinRequested(uint64_t aLobbyId, uint64_t aFriendId) noexcept;
    void OnRichPresenceJoinRequested(uint64_t aFriendId, const char* acConnect) noexcept;
    void OnLobbyInvite(uint64_t aFriendId, uint64_t aLobbyId) noexcept;

    struct Invite
    {
        uint64_t FriendId{};
        uint64_t LobbyId{};
    };

    World& m_world;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
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
    TiltedPhoques::TaskQueue m_titleScreenTasks;
    std::vector<std::unique_ptr<CallbackBridge>> m_callbacks;
    std::vector<Invite> m_invites;          // incoming, newest last
    std::unordered_set<uint64_t> m_invited; // friends invited from this lobby
    std::unordered_set<uint64_t> m_avatarsSent;
    uint64_t m_richPresenceLobby{~0ull};
    uint64_t m_launchLobby{};               // +connect_lobby from the command line
    mutable std::mutex m_testStateLock;
    std::string m_testState{"{}"};
};
